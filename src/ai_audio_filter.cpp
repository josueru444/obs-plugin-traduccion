#include "ai_audio_filter.h"
#include "ai_audio_filter.h"
#include "ai_audio_filter.h"
#include "ai_audio_filter.h"
#include "ai_audio_filter.h"

#include "audio_processor.h"

#include <string.h>
#include <thread>
#include <mutex>
#include <vector>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <cmath>
#include <algorithm>
#include <cctype>

static const float SILENCE_RMS_THRESHOLD = 0.0015f;
static const size_t MIN_SPEECH_MS = 300;
static const size_t SILENCE_HANGOVER_MS = 600;
static const size_t MAX_SEGMENT_MS = 15000;
static const size_t PREROLL_MS = 200;

struct VadState {
	bool speaking = false;
	std::vector<float> speech_frames;
	std::vector<float> preroll;
	size_t silence_ms = 0;
	size_t speech_ms = 0;
	size_t last_partial_ms = 0;
	size_t sentence_id = 0;
};

struct AudioSegment {
	std::vector<float> audio;
	bool is_final;
	size_t sentence_id;
};

struct ai_filter_data {
	audio_processor *processor;
	VadState vad;
	std::queue<AudioSegment> segment_queue;
	std::mutex queue_mutex;
	std::condition_variable cv;
	std::thread worker_thread;
	std::atomic<bool> stop_worker;
	std::string current_language;
	bool local_translation;
	bool use_gpu;
	std::string current_model_path;

	// store translation text
	std::string target_source_name;
	// VAD
	struct whisper_vad_context *vad_ctx;
};

// limit text historythe last N characters and let OBS handle the word wrap
static std::string format_subtitles(const std::string &text, size_t max_chars = 150)
{
	if (text.length() <= max_chars)
		return text;

	// avoid cutting a word in half, find the first space AFTER our cut point
	size_t start_pos = text.length() - max_chars;
	size_t space_pos = text.find_first_of(" \t\n", start_pos);

	if (space_pos != std::string::npos && space_pos < text.length() - 1) {
		return text.substr(space_pos + 1);
	}

	return text.substr(start_pos);
}

// remove tags like [Music] or (silence)
static std::string sanitize_text(const std::string &text)
{
	std::string result = "";
	bool in_bracket = false;
	bool in_paren = false;
	bool in_asterisk = false;

	for (char c : text) {
		if (c == '[')
			in_bracket = true;
		else if (c == ']') {
			in_bracket = false;
			continue;
		} else if (c == '(')
			in_paren = true;
		else if (c == ')') {
			in_paren = false;
			continue;
		} else if (c == '*') {
			in_asterisk = !in_asterisk;
			continue;
		}

		if (!in_bracket && !in_paren && !in_asterisk) {
			result += c;
		}
	}

	//  Trim whitespace
	size_t start = result.find_first_not_of(" \t\n\r");
	if (start == std::string::npos)
		return "";
	size_t end = result.find_last_not_of(" \t\n\r");
	result = result.substr(start, end - start + 1);

	// Filtro adicional para alucinaciones comunes de Whisper
	std::string clean = "";
	for (char c : result) {
		if (std::isalpha(c) || c == ' ') clean += std::tolower(c);
	}
	if (clean == "the end" || clean == "subscribe" || clean == "sign up at domestikaorg" || 
	    clean == "im sorry im sorry" || clean == "im not sure what im doing here" || 
	    clean == "gracias" || clean == "thank you" || clean == "thanks for watching") {
		return "";
	}

	return result;
}

static void transcription_worker(ai_filter_data *data)
{
	while (!data->stop_worker.load()) {
		AudioSegment segment;
		{
			std::unique_lock<std::mutex> lock(data->queue_mutex);
			data->cv.wait(lock,
				      [data] { return !data->segment_queue.empty() || data->stop_worker.load(); });

			if (data->stop_worker.load() && data->segment_queue.empty()) {
				break;
			}

			segment = data->segment_queue.front();
			data->segment_queue.pop();

			while (!segment.is_final && !data->segment_queue.empty()) {
				AudioSegment next_seg = data->segment_queue.front();

				if (!next_seg.is_final && next_seg.sentence_id == segment.sentence_id) {
					segment = next_seg;
					data->segment_queue.pop();
				} else {
					break;
				}
			}
		}

		if (!segment.audio.empty()) {
			std::string prompt = segment.is_final ? "Vocabulario coloquial mexicano: wey, no mames, pinche, cabrón, pendejo." : "";
			std::string raw_texto = data->processor->process_audio(segment.audio, data->current_language,
									       data->local_translation, prompt);
			std::string texto = sanitize_text(raw_texto);
			if (!texto.empty()) {
				blog(LOG_INFO, "[Traductor IA] Transcripción (%s): %s",
				     segment.is_final ? "FINAL" : "PARCIAL", texto.c_str());

				// automatically search for the "fuente_subtitulos_ia" source
				obs_source_t *custom_source = nullptr;
				obs_enum_sources(
					[](void *data, obs_source_t *source) {
						obs_source_t **found = (obs_source_t **)data;
						if (strcmp(obs_source_get_unversioned_id(source),
							   "fuente_subtitulos_ia") == 0) {
							*found = obs_source_get_ref(source);
							return false; // stop search after finding the source
						}
						return true; // continue searching
					},
					&custom_source);

				if (custom_source != nullptr) {
					obs_data_t *new_settings = obs_data_create();

					// add 3 dots if partial
					std::string texto_final = segment.is_final ? texto : texto + "...";

					// limit history~150 characterslet OBS do Word Wrap
					std::string texto_formateado = format_subtitles(texto_final, 150);

					obs_data_set_string(new_settings, "text", texto_formateado.c_str());
					obs_source_update(custom_source, new_settings);
					obs_data_release(new_settings);
					obs_source_release(custom_source);
				}
			}
		}
	}
}

// search automatically, the search_ai_subtitle_source function is no longer needed

static bool btn_save(obs_properties_t *props, obs_property_t *property, void *data)
{
	(void)props;
	(void)property;
	(void)data;
	return true;
}

static bool on_external_server_toggled(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	(void)p;
	// read if external server group is enabled
	bool use_external = obs_data_get_bool(settings, "use_external_server");

	// find the checkbox and help text for local translation
	obs_property_t *local_trans = obs_properties_get(props, "local_translation");
	obs_property_t *trans_help = obs_properties_get(props, "trans_help");

	// hide local translation if external server is used
	if (local_trans)
		obs_property_set_visible(local_trans, !use_external);
	if (trans_help)
		obs_property_set_visible(trans_help, !use_external);

	return true; // force OBSredraw the window
}

obs_properties_t *ai_filter_get_properties(void *data)
{
	(void)data;
	obs_properties_t *props = obs_properties_create();

	//  --- SECTION 1: AI ENGINE ---
	obs_properties_t *group_model = obs_properties_create();

	obs_property_t *combo_model = obs_properties_add_list(
		group_model, "model_settings", "Modelo predeterminado:", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(combo_model, "Tiny (Rápido)", "ggml-tiny.bin");
	obs_property_list_add_string(combo_model, "Base (Balanceado)", "ggml-base.bin");
	obs_property_list_add_string(combo_model, "Small (Alta Precisión)", "ggml-small.bin");
	obs_properties_add_text(group_model, "model_help",
				"Modelos más grandes (Small) ofrecen mayor precisión pero consumen más recursos.",
				OBS_TEXT_INFO);

	obs_properties_add_bool(group_model, "use_custom_model", "Usar modelo personalizado");
	obs_properties_add_path(group_model, "custom_model_path", "O usa un modelo local (.bin):", OBS_PATH_FILE,
				"Modelos Whisper (*.bin);;Todos los archivos (*.*)", NULL);

	obs_properties_add_bool(group_model, "processing_mode", "Usar Tarjeta de Video (GPU)");
	obs_properties_add_text(group_model, "gpu_help",
				"Nota: Si falla la transcripción, desmarca esta casilla para usar tu procesador.",
				OBS_TEXT_INFO);

	obs_properties_add_group(props, "grp_models", "1. Motor de Transcripción (Whisper)", OBS_GROUP_NORMAL,
				 group_model);

	//  --- SECTION 2: LANGUAGES AND QUICK TRANSLATION ---
	obs_properties_t *group_translation = obs_properties_create();

	obs_property_t *combo_in = obs_properties_add_list(
		group_translation, "lang_in", "Idioma que vas a hablar:", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(combo_in, "Automático", "auto");
	obs_property_list_add_string(combo_in, "Español", "es");
	obs_property_list_add_string(combo_in, "Inglés", "en");

	obs_properties_add_bool(group_translation, "local_translation", "Traducir al Inglés (Modelo Local)");
	obs_properties_add_text(group_translation, "trans_help",
				"Nota: El modelo local solo soporta traducción hacia el Inglés.", OBS_TEXT_INFO);

	obs_properties_add_group(props, "grp_translation", "2. Idioma de Entrada", OBS_GROUP_NORMAL, group_translation);

	//  --- SECTION 3: EXTERNAL SERVER (LM Studio) ---
	obs_properties_t *group_connection = obs_properties_create();
	obs_properties_add_text(group_connection, "server_url", "URL de LM Studio:", OBS_TEXT_DEFAULT);
	obs_properties_add_int(group_connection, "server_port", "Puerto:", 1, 65535, 1);

	obs_property_t *combo_out = obs_properties_add_list(group_connection, "lang_out",
							    "Traducir hacia (Idioma de salida):", OBS_COMBO_TYPE_LIST,
							    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(combo_out, "Español", "es");
	obs_property_list_add_string(combo_out, "Inglés", "en");

	// act as a Checkbox that turns it on/off since it is OBS_GROUP_CHECKABLE
	obs_property_t *ext_group = obs_properties_add_group(props, "use_external_server",
							     "3. Usar Servidor Externo (Traducciones Complejas)",
							     OBS_GROUP_CHECKABLE, group_connection);
	obs_property_set_modified_callback(ext_group, on_external_server_toggled);

	//  --- FINAL BUTTON ---
	obs_properties_add_button(props, "btn_save", "Aplicar Cambios", btn_save);

	return props;
}

// return the filter name
static const char *ai_filter_get_name(void *data)
{
	(void)data;
	return "Traductor IA";
}

static void ai_filter_update(void *data, obs_data_t *settings)
{
	// unpack our filter memory
	ai_filter_data *filter_data = static_cast<ai_filter_data *>(data);

	// save all UI values in our filter
	filter_data->current_language = obs_data_get_string(settings, "lang_in");
	filter_data->use_gpu = obs_data_get_bool(settings, "processing_mode");

	// transcribe with local model
	filter_data->local_translation = obs_data_get_bool(settings, "local_translation");

	// save the previous pathcheck if it changed
	std::string old_path = filter_data->current_model_path;

	// choose between Combo Box or Custom Path
	bool use_custom = obs_data_get_bool(settings, "use_custom_model");
	const char *custom_path = obs_data_get_string(settings, "custom_model_path");

	if (use_custom && custom_path && custom_path[0] != '\0') {
		// use manual path if provided
		filter_data->current_model_path = custom_path;
	} else {
		// search the packed model from the combo box if no manual path
		const char *model_size = obs_data_get_string(settings, "model_settings");
		char relative_path[256];
		snprintf(relative_path, sizeof(relative_path), "models/%s", model_size);

		char *absolute_model_path = obs_module_file(relative_path);
		if (absolute_model_path) {
			filter_data->current_model_path = absolute_model_path;
			bfree(absolute_model_path);
		} else {
			blog(LOG_ERROR, "Error: No se encontro el modelo %s en la carpeta data", relative_path);
		}
	}
	blog(LOG_INFO, "[Traductor IA] Ruta del modelo a usar: %s", filter_data->current_model_path.c_str());

	// hot-swap the processor if the path changed and the processor exists
	if (filter_data->processor != nullptr && old_path != filter_data->current_model_path) {
		blog(LOG_INFO, "[Traductor IA] Detectado cambio de modelo. Reiniciando motor IA...");

		// stop the current thread safely
		filter_data->stop_worker.store(true);
		filter_data->cv.notify_all();
		if (filter_data->worker_thread.joinable()) {
			filter_data->worker_thread.join();
		}

		// destroy the old processor and free memory
		delete filter_data->processor;

		// create a new processor with the new path
		filter_data->processor = new audio_processor(filter_data->current_model_path);

		// restart the worker thread
		filter_data->stop_worker.store(false);
		filter_data->worker_thread = std::thread(transcription_worker, filter_data);

		blog(LOG_INFO, "[Traductor IA] Nuevo modelo cargado en caliente y listo.");
	}
}

// provide the required definition
static void *ai_filter_create(obs_data_t *settings, obs_source_t *source)
{
	(void)source;
	ai_filter_data *data = new ai_filter_data();
	data->stop_worker.store(false);
	ai_filter_update(data, settings);

	data->processor = new audio_processor(data->current_model_path);

	data->worker_thread = std::thread(transcription_worker, data);

	// load VAD model
	struct whisper_vad_context_params vad_params = whisper_vad_default_context_params();
	char *vad_model_path = obs_module_file("models/silero_vad.bin");
	if (vad_model_path) {
		data->vad_ctx = whisper_vad_init_from_file_with_params(vad_model_path, vad_params);
		bfree(vad_model_path);
		blog(LOG_INFO, "[Traductor IA] VAD de Silero inicializado correctamente.");
	} else {
		blog(LOG_ERROR, "[Traductor IA] Error: No se encontró el modelo VAD silero_vad.bin en models/");
		data->vad_ctx = nullptr;
	}

	return data;
}

static void ai_filter_destroy(void *data)
{
	ai_filter_data *filter_data = static_cast<ai_filter_data *>(data);

	filter_data->stop_worker.store(true);
	filter_data->cv.notify_all();
	if (filter_data->worker_thread.joinable()) {
		filter_data->worker_thread.join();
	}

	if (filter_data->processor) {
		delete filter_data->processor;
	}
	// destroying vad
	if (filter_data->vad_ctx) {
		whisper_vad_free(filter_data->vad_ctx);
	}

	delete filter_data;
}

static void aI_filter_get_default(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "lang_in", "es");
	obs_data_set_default_string(settings, "lang_out", "en");

	obs_data_set_default_string(settings, "model_settings", "ggml-base.bin");
	obs_data_set_default_string(settings, "custom_model_path", ""); // set empty path by default
	obs_data_set_default_bool(settings, "use_custom_model", false);
	obs_data_set_default_bool(settings, "processing_mode", false);

	// set default local translation
	obs_data_set_default_bool(settings, "local_translation", false);
}

static void _flush_segment(ai_filter_data *filter_data)
{
	if (filter_data->vad.speech_ms >= MIN_SPEECH_MS) {
		AudioSegment seg;
		seg.audio = filter_data->vad.speech_frames;
		seg.is_final = true;
		seg.sentence_id = filter_data->vad.sentence_id;

		{
			std::lock_guard<std::mutex> lock(filter_data->queue_mutex);
			filter_data->segment_queue.push(seg);
		}
		filter_data->cv.notify_one();

		filter_data->vad.sentence_id++;
	}
	filter_data->vad.speaking = false;
	filter_data->vad.speech_frames.clear();
	filter_data->vad.speech_ms = 0;
	filter_data->vad.silence_ms = 0;
	filter_data->vad.last_partial_ms = 0;
}

static struct obs_audio_data *ai_filter_audio(void *data, struct obs_audio_data *audio)
{
	ai_filter_data *filter_data = static_cast<ai_filter_data *>(data);
	if (filter_data->processor && audio->data[0]) {
		float *raw_audio = (float *)audio->data[0];
		size_t num_samples = audio->frames;
		struct obs_audio_info oai;
		if (obs_get_audio_info(&oai)) {
			std::vector<float> pcmf32;

			if (oai.samples_per_sec != 16000) {
				double ratio = (double)oai.samples_per_sec / 16000.0;
				size_t out_frames = (size_t)(num_samples / ratio);
				pcmf32.reserve(out_frames);
				for (size_t i = 0; i < out_frames; ++i) {
					double pos = i * ratio;
					size_t index = (size_t)pos;
					double frac = pos - index;
					if (index + 1 < num_samples) {
						pcmf32.push_back((float)(raw_audio[index] * (1.0 - frac) + raw_audio[index + 1] * frac));
					} else {
						pcmf32.push_back(raw_audio[index]);
					}
				}
			} else {
				pcmf32.assign(raw_audio, raw_audio + num_samples);
			}

			if (!pcmf32.empty()) {
				float sum = 0.0f;
				for (float sample : pcmf32) {
					sum += sample * sample;
				}
				float rms = std::sqrt(sum / pcmf32.size());

				bool is_speech = false;
				if (rms > SILENCE_RMS_THRESHOLD) {
					is_speech = whisper_vad_detect_speech_no_reset(filter_data->vad_ctx, pcmf32.data(),
											    pcmf32.size());
				}

				size_t frame_ms = (pcmf32.size() * 1000) / 16000;

				if (is_speech) {
					if (!filter_data->vad.speaking) {
						filter_data->vad.speaking = true;
						filter_data->vad.speech_frames = filter_data->vad.preroll;
						filter_data->vad.speech_ms =
							(filter_data->vad.speech_frames.size() * 1000) / 16000;
					}

					filter_data->vad.speech_frames.insert(filter_data->vad.speech_frames.end(),
									      pcmf32.begin(), pcmf32.end());
					filter_data->vad.speech_ms += frame_ms;
					filter_data->vad.silence_ms = 0;

					if (filter_data->vad.speech_ms - filter_data->vad.last_partial_ms >= 1000) {
						filter_data->vad.last_partial_ms = filter_data->vad.speech_ms;

						AudioSegment seg;
						seg.audio = filter_data->vad.speech_frames;
						seg.is_final = false;
						seg.sentence_id = filter_data->vad.sentence_id;

						{
							std::lock_guard<std::mutex> lock(filter_data->queue_mutex);
							filter_data->segment_queue.push(seg);
						}
						filter_data->cv.notify_one();
					}
				} else {
					filter_data->vad.preroll.insert(filter_data->vad.preroll.end(), pcmf32.begin(),
									pcmf32.end());
					if (filter_data->vad.preroll.size() > (PREROLL_MS * 16)) {
						filter_data->vad.preroll.erase(
							filter_data->vad.preroll.begin(),
							filter_data->vad.preroll.begin() +
								(filter_data->vad.preroll.size() - (PREROLL_MS * 16)));
					}

					if (filter_data->vad.speaking) {
						filter_data->vad.speech_frames.insert(
							filter_data->vad.speech_frames.end(), pcmf32.begin(),
							pcmf32.end());
						filter_data->vad.silence_ms += frame_ms;

						if (filter_data->vad.silence_ms >= SILENCE_HANGOVER_MS) {
							_flush_segment(filter_data);
						} else if (filter_data->vad.speech_ms -
								   filter_data->vad.last_partial_ms >=
							   1000) {
							filter_data->vad.last_partial_ms = filter_data->vad.speech_ms;
							AudioSegment seg;
							seg.audio = filter_data->vad.speech_frames;
							seg.is_final = false;
							seg.sentence_id = filter_data->vad.sentence_id;

							{
								std::lock_guard<std::mutex> lock(
									filter_data->queue_mutex);
								filter_data->segment_queue.push(seg);
							}
							filter_data->cv.notify_one();
						}
					}
				}

				if (filter_data->vad.speaking && filter_data->vad.speech_ms >= MAX_SEGMENT_MS) {
					_flush_segment(filter_data);
				}
			}
		}
	}

	return audio;
}

extern "C" struct obs_source_info get_ai_filter_info()
{
	struct obs_source_info info = {0};
	info.id = "ai_translation_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = ai_filter_get_name;

	info.get_defaults = aI_filter_get_default;
	info.update = ai_filter_update;

	info.create = ai_filter_create;
	info.destroy = ai_filter_destroy;
	info.get_properties = ai_filter_get_properties;
	info.filter_audio = ai_filter_audio;

	return info;
}