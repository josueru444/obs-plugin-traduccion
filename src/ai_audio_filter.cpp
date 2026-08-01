#include "ai_audio_filter.h"
#include "audio_processor.h"
#include "remote_transcriber.h"

#include <media-io/audio-resampler.h>
#include <string.h>
#include <thread>
#include <mutex>
#include <vector>
#include <atomic>
#include <queue>
#include <deque>
#include <condition_variable>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// VAD constants
// ─────────────────────────────────────────────────────────────────────────────
static const float SILENCE_RMS_THRESHOLD = 0.003f;
static const size_t MIN_SPEECH_MS = 500;
static const size_t SILENCE_HANGOVER_MS = 600;
static const size_t MAX_SEGMENT_MS = 15000;
static const size_t PREROLL_MS = 200;
static const size_t OVERLAP_MS = 200;

// Maximum confirmed sentences kept in the subtitle ring buffer.
// The subtitle source's own max_lines UI setting further limits how many
// lines are actually rendered on screen, so the user stays in control.
static const size_t MAX_CONFIRMED_LINES = 6;

// ─────────────────────────────────────────────────────────────────────────────
// Data structures
// ─────────────────────────────────────────────────────────────────────────────
struct VadState {
	bool speaking = false;
	std::vector<float> speech_frames;
	std::vector<float> preroll;
	std::vector<float> overlap_buffer;
	size_t silence_ms = 0;
	size_t speech_ms = 0;
	size_t last_partial_ms = 0;
	size_t sentence_id = 0;
};

struct AudioSegment {
	std::vector<float>* audio;
	bool is_final;
	size_t sentence_id;
};

struct ai_filter_data {
	// ── Transcription backend (only one active at a time) ─────────────────────
	audio_processor *processor;       // Local mode: Whisper (nullptr in remote mode)
	RemoteTranscriber *remote_client; // Remote mode: WebSocket (nullptr in local mode)

	// ── Configuration ─────────────────────────────────────────────────────────
	bool use_remote_transcription; // true = send audio to WebSocket server
	std::string ws_url;            // WebSocket server base URL
	std::string ws_token;          // WebSocket authentication token (optional)
	std::string connection_status{"🔴 Desconectado"};
	std::mutex status_mutex;
	obs_source_t *self_source{nullptr};
	std::string current_language;
	std::string target_language;
	bool local_translation;
	bool use_gpu;
	int whisper_threads;
	std::string current_model_path;
	std::string target_source_name;

	// ── VAD and audio pipeline ────────────────────────────────────────────────
	VadState vad;
	std::queue<AudioSegment> segment_queue;
	std::mutex queue_mutex;
	std::condition_variable cv;
	std::thread worker_thread;
	std::atomic<bool> stop_worker;

	std::vector<std::vector<float>*> buffer_pool;
	std::mutex pool_mutex;

	audio_resampler_t *resampler;
	uint32_t resampler_src_rate;

	struct whisper_vad_context *vad_ctx;

	// ── Subtitle text accumulation ────────────────────────────────────────
	// confirmed_lines : ring buffer of final sentences (oldest → newest).
	// current_partial : latest partial text in progress (shown with "...").
	// last_displayed  : last string sent to the OBS source; used to skip
	//                   redundant updates when the text hasn't changed.
	// subtitle_mutex  : guards all three fields (callback and worker threads).
	std::deque<std::string> confirmed_lines;
	std::string             current_partial;
	std::string             last_displayed;
	std::mutex              subtitle_mutex;

	// ── Auto-clear & Performance ──────────────────────────────────────────
	obs_weak_source_t *subtitle_weak_ref{nullptr};
	std::chrono::steady_clock::time_point last_subtitle_time;
	std::atomic<bool> subtitle_visible{false};
	int auto_clear_seconds{5};
};

// Build full WebSocket URL with token parameter if present
static std::string build_full_ws_url(const std::string &url, const std::string &token, const std::string &lang_in, const std::string &lang_out)
{
	auto trim_string = [](std::string s) {
		s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
		return s;
	};

	std::string clean_url = trim_string(url);
	std::string clean_token = trim_string(token);

	if (clean_url.empty()) return clean_url;
	std::string full_url = clean_url;
	bool has_query = (full_url.find('?') != std::string::npos);

	auto append_param = [&](const std::string &key, const std::string &val) {
		if (val.empty()) return;
		// Do not duplicate parameter if already present in full_url
		if (has_query && (full_url.find("?" + key + "=") != std::string::npos || full_url.find("&" + key + "=") != std::string::npos))
			return;

		if (has_query) full_url += "&" + key + "=" + val;
		else { full_url += "?" + key + "=" + val; has_query = true; }
	};

	append_param("token", clean_token);
	append_param("lang_in", trim_string(lang_in));
	append_param("lang_out", trim_string(lang_out));

	return full_url;
}

// ─────────────────────────────────────────────────────────────────────────────
// Text helper functions
// ─────────────────────────────────────────────────────────────────────────────
static std::string format_subtitles(const std::string &text, size_t max_chars = 150)
{
	if (text.length() <= max_chars)
		return text;

	size_t start_pos = text.length() - max_chars;
	size_t space_pos = text.find_first_of(" \t\n", start_pos);

	if (space_pos != std::string::npos && space_pos < text.length() - 1)
		return text.substr(space_pos + 1);

	return text.substr(start_pos);
}

static bool is_repetitive(const std::string &text)
{
	std::vector<std::string> words;
	std::string current;
	for (char c : text) {
		if (std::isalnum(static_cast<unsigned char>(c))) {
			current += std::tolower(static_cast<unsigned char>(c));
		} else if (!current.empty()) {
			words.push_back(current);
			current.clear();
		}
	}
	if (!current.empty())
		words.push_back(current);

	if (words.size() < 4)
		return false;

	std::unordered_map<std::string, int> bigram_counts;
	for (size_t i = 0; i + 1 < words.size(); ++i) {
		std::string bigram = words[i] + " " + words[i + 1];
		bigram_counts[bigram]++;
		if (bigram_counts[bigram] >= 3)
			return true;
	}

	std::unordered_set<std::string> unique_words(words.begin(), words.end());
	float unique_ratio = (float)unique_words.size() / (float)words.size();
	if (unique_ratio < 0.35f)
		return true;

	return false;
}

static std::string sanitize_text(const std::string &text)
{
	std::string result;
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

		if (!in_bracket && !in_paren && !in_asterisk)
			result += c;
	}

	size_t start = result.find_first_not_of(" \t\n\r");
	if (start == std::string::npos)
		return "";
	size_t end = result.find_last_not_of(" \t\n\r");
	result = result.substr(start, end - start + 1);

	if (result.length() <= 2)
		return "";

	std::string lower_res;
	for (char c : result) {
		lower_res += std::tolower(static_cast<unsigned char>(c));
	}
	if (lower_res.find("thanks for watching") != std::string::npos ||
	    lower_res.find("subtitles by") != std::string::npos ||
	    lower_res.find("subtitled by") != std::string::npos ||
	    lower_res.find("amara.org") != std::string::npos ||
	    lower_res.find("subscribe") != std::string::npos ||
	    lower_res.find("suscríbete") != std::string::npos) {
		return "";
	}

	if (is_repetitive(result))
		return "";

	return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// update_subtitle_source
//
// Accumulates confirmed final sentences in a rolling ring buffer and keeps
// the current partial as a live "in-progress" line. The subtitle source's
// own max_lines setting (configurable by the user) controls how many lines
// are ultimately rendered on screen.
//
// Behaviour:
//   is_final=true  → push text to confirmed_lines, clear current_partial.
//   is_final=false → update current_partial only (no new line added).
//   Either path    → skip the OBS source write if the display text is
//                    unchanged (Mejora 3: avoids flicker on identical text).
//
// The text NEVER auto-clears; it stays visible until new content replaces it.
// ─────────────────────────────────────────────────────────────────────────────
static void update_subtitle_source(ai_filter_data *data, const std::string &text, bool is_final)
{
	// ── 1. Update the accumulation buffer (thread-safe) ───────────────────
	std::string display;
	{
		std::lock_guard<std::mutex> lock(data->subtitle_mutex);

		if (is_final) {
			if (!text.empty()) {
				// Trim individual sentence before storing (safety cap).
				data->confirmed_lines.push_back(format_subtitles(text, 150));
				// Evict oldest entry when ring buffer is full.
				while (data->confirmed_lines.size() > MAX_CONFIRMED_LINES)
					data->confirmed_lines.pop_front();
			} else {
				// Empty final text implies a clear command
				data->confirmed_lines.clear();
			}
			data->current_partial.clear();
		} else {
			data->current_partial = text;
		}

		// ── 2. Build combined display string ─────────────────────────────
		// Confirmed lines come first (oldest → newest); partial appended last.
		// Newline separators let the subtitle source apply its max_lines cap.
		for (const auto &line : data->confirmed_lines) {
			if (!display.empty())
				display += '\n';
			display += line;
		}
		if (!data->current_partial.empty()) {
			if (!display.empty())
				display += '\n';
			display += format_subtitles(data->current_partial, 150);
		}

		// ── 3. Skip OBS update if nothing changed ────────────────────────
		if (display == data->last_displayed)
			return;
		data->last_displayed = display;
		
		if (!display.empty()) {
			data->last_subtitle_time = std::chrono::steady_clock::now();
			data->subtitle_visible = true;
		}
	}

	// ── 4. Push combined text to OBS source (outside the mutex) ──────────
	obs_source_t *custom_source = nullptr;
	if (data->subtitle_weak_ref) {
		custom_source = obs_weak_source_get_source(data->subtitle_weak_ref);
		if (!custom_source) {
			obs_weak_source_release(data->subtitle_weak_ref);
			data->subtitle_weak_ref = nullptr;
		}
	}
	
	if (!custom_source) {
		obs_enum_sources(
			[](void *param, obs_source_t *source) {
				obs_source_t **found = (obs_source_t **)param;
				if (strcmp(obs_source_get_unversioned_id(source), "fuente_subtitulos_ia") == 0) {
					*found = obs_source_get_ref(source);
					return false;
				}
				return true;
			},
			&custom_source);
		if (custom_source) {
			data->subtitle_weak_ref = obs_source_get_weak_source(custom_source);
		}
	}

	if (custom_source != nullptr) {
		obs_data_t *new_settings = obs_data_create();
		obs_data_set_string(new_settings, "text", display.c_str());
		obs_source_update(custom_source, new_settings);
		obs_data_release(new_settings);
		obs_source_release(custom_source);
	}
}



// ─────────────────────────────────────────────────────────────────────────────
// transcription_worker
//
// Dedicated worker thread separate from OBS audio thread.
// Read audio segments from queue and dispatch to active backend:
//   - Local mode  -> call Whisper and update subtitle directly.
//   - Remote mode -> encode to Opus, send via WebSocket, and return.
//                    Response arrives asynchronously in on_message().
// ─────────────────────────────────────────────────────────────────────────────
static void transcription_worker(ai_filter_data *data)
{
	while (!data->stop_worker.load()) {
		AudioSegment segment;
		{
			std::unique_lock<std::mutex> lock(data->queue_mutex);
			bool signaled = data->cv.wait_for(lock, std::chrono::milliseconds(500), [data] {
				return !data->segment_queue.empty() || data->stop_worker.load();
			});

			if (!signaled) {
				if (data->subtitle_visible.load() && data->auto_clear_seconds > 0) {
					auto now = std::chrono::steady_clock::now();
					auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - data->last_subtitle_time).count();
					
					if (duration >= data->auto_clear_seconds) {
						data->subtitle_visible.store(false);
						update_subtitle_source(data, "", true);
					}
				}
				continue;
			}

			if (data->stop_worker.load() && data->segment_queue.empty())
				break;

			segment = data->segment_queue.front();
			data->segment_queue.pop();

			// Discard obsolete partial segments for same sentence_id
			while (!segment.is_final && !data->segment_queue.empty()) {
				AudioSegment &next = data->segment_queue.front();
				if (!next.is_final && next.sentence_id == segment.sentence_id) {
					if (segment.audio) {
						std::lock_guard<std::mutex> pool_lock(data->pool_mutex);
						data->buffer_pool.push_back(segment.audio);
					}
					segment = next;
					data->segment_queue.pop();
				} else {
					break;
				}
			}
		}

		if (!segment.audio || segment.audio->empty()) {
			if (segment.audio) {
				std::lock_guard<std::mutex> pool_lock(data->pool_mutex);
				data->buffer_pool.push_back(segment.audio);
			}
			continue;
		}

		if (data->use_remote_transcription && data->remote_client) {
			// ── Remote mode ───────────────────────────────────────────────────
			// Obsolete partials for the same sentence_id were already drained from
			// the queue above (lines 256-264), so we send only the most recent
			// audio snapshot. The backend applies a second layer of backpressure
			// (drops partial if Whisper is already busy), so any excess partials
			// that still reach the server are silently discarded there.
			blog(LOG_DEBUG,
			     "[AI Translator] -> Remote: send segment %zu (%s, %zu samples)",
			     segment.sentence_id, segment.is_final ? "FINAL" : "PARTIAL",
			     segment.audio->size());
			data->remote_client->send_audio(*segment.audio, segment.sentence_id,
			                                segment.is_final);

		} else if (data->processor) {
			// ── Local mode (Whisper) ──────────────────────────────────────────
			std::string raw_texto = data->processor->process_audio(
				*segment.audio, data->current_language, data->local_translation, "",
				data->whisper_threads);
			std::string texto = sanitize_text(raw_texto);



			if (!texto.empty()) {
				update_subtitle_source(data, texto, segment.is_final);
			}
		}

		// Return buffer to pool
		if (segment.audio) {
			segment.audio->clear();
			std::lock_guard<std::mutex> pool_lock(data->pool_mutex);
			data->buffer_pool.push_back(segment.audio);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Property callbacks
// ─────────────────────────────────────────────────────────────────────────────

// Refresh connection status manually from the UI
// Connect button: reads current URL+Token from settings and connects immediately
static bool on_connect_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	(void)props;
	(void)p;
	ai_filter_data *fd = static_cast<ai_filter_data *>(data);
	if (!fd || !fd->remote_client)
		return false;

	std::string full_url = build_full_ws_url(fd->ws_url, fd->ws_token,
	                                          fd->current_language, fd->target_language);

	if (full_url.empty()) {
		blog(LOG_INFO, "[AI Translator] Connect button pressed with empty URL -> Disconnecting");
	} else {
		blog(LOG_INFO, "[AI Translator] Connect button pressed -> %s", full_url.c_str());
	}
	fd->remote_client->update_url(full_url);

	bool url_changed = (full_url != fd->remote_client->get_url());
	bool is_connected = fd->remote_client->is_connected();

	// Optimistically update the UI to show that an action is taking place.
	// The true status will be updated asynchronously.
	std::string optimistic_status;
	if (full_url.empty()) {
		optimistic_status = "🔴 Desconectado";
	} else if (url_changed || !is_connected) {
		optimistic_status = "🟡 Conectando / Verificando...";
	} else {
		// URL didn't change and we are already connected. Fetch the true status from the background thread safely.
		std::lock_guard<std::mutex> lock(fd->status_mutex);
		optimistic_status = fd->connection_status;
	}

	obs_property_t *status_prop = obs_properties_get(props, "status_label");
	if (status_prop) {
		obs_property_set_description(status_prop, ("Estado: " + optimistic_status).c_str());
	}

	return true; // Force UI refresh
}

// Hide Whisper property group when remote transcription is enabled.
static bool on_remote_transcription_toggled(obs_properties_t *props, obs_property_t *p,
                                             obs_data_t *settings)
{
	(void)p;
	bool use_remote = obs_data_get_bool(settings, "use_remote_transcription");
	obs_property_t *model_group = obs_properties_get(props, "grp_models");
	if (model_group)
		obs_property_set_visible(model_group, !use_remote);
	return true;
}



// ─────────────────────────────────────────────────────────────────────────────
// Filter properties (UI)
// ─────────────────────────────────────────────────────────────────────────────
obs_properties_t *ai_filter_get_properties(void *data)
{
	(void)data;
	obs_properties_t *props = obs_properties_create();

	// ── Group 1: Local Transcription Engine (Whisper) ─────────────────────────
	obs_properties_t *group_model = obs_properties_create();

	obs_property_t *combo_model =
		obs_properties_add_list(group_model, "model_settings", "Modelo predeterminado:",
		                        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(combo_model, "Tiny (Rápido)", "ggml-tiny.bin");
	obs_property_list_add_string(combo_model, "Base (Balanceado)", "ggml-base.bin");
	obs_property_list_add_string(combo_model, "Small (Alta Precisión)", "ggml-small.bin");
	obs_properties_add_text(
		group_model, "model_help",
		"Modelos más grandes (Small) ofrecen mayor precisión pero consumen más recursos.",
		OBS_TEXT_INFO);

	obs_properties_add_bool(group_model, "use_custom_model", "Usar modelo personalizado");
	obs_properties_add_path(group_model, "custom_model_path", "O usa un modelo local (.bin):",
	                        OBS_PATH_FILE,
	                        "Modelos Whisper (*.bin);;Todos los archivos (*.*)", NULL);

	obs_properties_add_int(group_model, "whisper_threads", "Hilos de CPU:", 1, 8, 1);

	obs_properties_add_bool(group_model, "processing_mode", "Usar Tarjeta de Video (GPU)");
	obs_properties_add_text(
		group_model, "gpu_help",
		"Nota: Si falla la transcripción, desmarca esta casilla para usar tu procesador.",
		OBS_TEXT_INFO);

	obs_properties_add_group(props, "grp_models", "1. Motor de Transcripción Local (Whisper)",
	                          OBS_GROUP_NORMAL, group_model);

	// ── Group 2: Input Language ───────────────────────────────────────────────
	obs_properties_t *group_translation = obs_properties_create();

	obs_property_t *combo_in =
		obs_properties_add_list(group_translation, "lang_in", "Idioma que vas a hablar:",
		                        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(combo_in, "Automático", "auto");
	obs_property_list_add_string(combo_in, "Español", "es");
	obs_property_list_add_string(combo_in, "Inglés", "en");

	obs_property_t *combo_out =
		obs_properties_add_list(group_translation, "lang_out", "Idioma de Traducción (Salida):",
		                        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(combo_out, "Mismo que el original", "original");
	obs_property_list_add_string(combo_out, "Inglés", "en");
	obs_property_list_add_string(combo_out, "Español", "es");

	obs_properties_add_text(
		group_translation, "trans_help",
		"Nota: El motor local (Whisper) solo soporta traducir hacia el Inglés. El servidor remoto soporta todos.", OBS_TEXT_INFO);

	obs_properties_add_group(props, "grp_translation", "2. Idioma de Entrada / Salida", OBS_GROUP_NORMAL,
	                         group_translation);

	// ── Group 3: Remote Transcription via WebSocket ───────────────────────────
	obs_properties_t *group_remote = obs_properties_create();

	obs_properties_add_text(group_remote, "ws_url", "URL del servidor WebSocket:",
	                        OBS_TEXT_DEFAULT);
	obs_properties_add_text(group_remote, "ws_token", "Token de Autenticación (Opcional):",
	                        OBS_TEXT_DEFAULT);

	std::string status_msg = "🔴 Desconectado";
	if (data) {
		ai_filter_data *fd = static_cast<ai_filter_data *>(data);
		if (fd->use_remote_transcription) {
			std::lock_guard<std::mutex> lock(fd->status_mutex);
			status_msg = fd->connection_status;
		} else {
			status_msg = "⚪ Inactivo (Modo local activo)";
		}
	}
	obs_properties_add_text(group_remote, "status_label", ("Estado: " + status_msg).c_str(),
	                        OBS_TEXT_INFO);
	obs_properties_add_button(group_remote, "connect_btn", "🔌 Conectar / Refrescar",
	                          on_connect_clicked);

	obs_properties_add_text(
		group_remote, "remote_info",
		"El servidor debe responder con JSON:\n"
		"{\"text\": \"...\", \"sentence_id\": N, \"is_final\": true}\n"
		"El audio se envía codificado en Opus (16kHz, mono, 24kbps).",
		OBS_TEXT_INFO);

	obs_property_t *remote_group = obs_properties_add_group(
		props, "use_remote_transcription", "3. Traducción Remota (WebSocket)",
		OBS_GROUP_CHECKABLE, group_remote);
	obs_property_set_modified_callback(remote_group, on_remote_transcription_toggled);

	obs_properties_add_int(props, "auto_clear_seconds", "Ocultar tras X segundos de silencio (0=nunca):", 0, 30, 1);

	return props;
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter name
// ─────────────────────────────────────────────────────────────────────────────
static const char *ai_filter_get_name(void *data)
{
	(void)data;
	return "Traductor IA";
}

// ─────────────────────────────────────────────────────────────────────────────
// ai_filter_update — Apply new configuration
//
// If mode or URL changes:
//   1. Stop worker thread
//   2. Destroy previous backend
//   3. Create new backend (Whisper or RemoteTranscriber)
//   4. Restart worker thread
// ─────────────────────────────────────────────────────────────────────────────
static void ai_filter_update(void *data, obs_data_t *settings)
{
	ai_filter_data *fd = static_cast<ai_filter_data *>(data);

	std::string old_lang_in = fd->current_language;
	std::string old_lang_out = fd->target_language;

	// Update basic settings (no backend restart required)
	fd->current_language = obs_data_get_string(settings, "lang_in");
	fd->target_language = obs_data_get_string(settings, "lang_out");
	fd->local_translation = (fd->target_language == "en");
	fd->use_gpu = obs_data_get_bool(settings, "processing_mode");
	fd->whisper_threads = (int)obs_data_get_int(settings, "whisper_threads");
	fd->auto_clear_seconds = (int)obs_data_get_int(settings, "auto_clear_seconds");

	// Determine Whisper model path
	std::string old_path = fd->current_model_path;
	std::string new_path;
	bool use_custom = obs_data_get_bool(settings, "use_custom_model");
	const char *custom_path = obs_data_get_string(settings, "custom_model_path");

	if (use_custom && custom_path && custom_path[0] != '\0') {
		new_path = custom_path;
	} else {
		const char *model_size = obs_data_get_string(settings, "model_settings");
		char rel[256];
		snprintf(rel, sizeof(rel), "models/%s", model_size);
		char *abs_path = obs_module_file(rel);
		if (abs_path) {
			new_path = abs_path;
			bfree(abs_path);
		} else {
			blog(LOG_ERROR, "[AI Translator] Model file not found: %s", rel);
		}
	}

	// Read remote mode settings
	std::string old_full_url = build_full_ws_url(fd->ws_url, fd->ws_token, old_lang_in, old_lang_out);
	bool old_use_remote = fd->use_remote_transcription;

	bool new_use_remote = obs_data_get_bool(settings, "use_remote_transcription");
	std::string new_ws_url = obs_data_get_string(settings, "ws_url");
	std::string new_ws_token = obs_data_get_string(settings, "ws_token");

	std::string new_full_url = build_full_ws_url(new_ws_url, new_ws_token, fd->current_language, fd->target_language);
	// Save updated values in data struct
	fd->current_model_path = new_path;
	fd->use_remote_transcription = new_use_remote;
	fd->ws_url = new_ws_url;
	fd->ws_token = new_ws_token;

	// ── Detect backend restart requirement ───────────────────────────────────
	bool has_backend = (fd->processor != nullptr || fd->remote_client != nullptr);
	bool mode_changed = has_backend && (new_use_remote != old_use_remote);
	bool path_changed = has_backend && !new_use_remote && (new_path != old_path);

	// If remote mode is active and only URL/Token changed, just save the values.
	// The user must press the "Conectar" button to apply the new URL.
	if (has_backend && !mode_changed && new_use_remote && fd->remote_client) {
		return;
	}

	if (!has_backend || (!mode_changed && !path_changed))
		return; // First call or no structural backend change

	blog(LOG_INFO, "[AI Translator] Reconfigure backend (mode=%s, path=%s)",
	     mode_changed ? "changed" : "-", path_changed ? "changed" : "-");

	// ── 1. Stop worker thread ────────────────────────────────────────────
	fd->stop_worker.store(true);
	fd->cv.notify_all();
	if (fd->worker_thread.joinable())
		fd->worker_thread.join();

	// Clear subtitle accumulation so the new backend starts from a clean slate.
	{
		std::lock_guard<std::mutex> lock(fd->subtitle_mutex);
		fd->confirmed_lines.clear();
		fd->current_partial.clear();
		fd->last_displayed.clear();
	}

	// ── 2. Destroy previous backend ──────────────────────────────────────────
	if (fd->remote_client) {
		delete fd->remote_client;
		fd->remote_client = nullptr;
	}
	if (fd->processor) {
		delete fd->processor;
		fd->processor = nullptr;
	}

	// ── 3. Create new backend ────────────────────────────────────────────────
	if (new_use_remote) {
		blog(LOG_INFO, "[AI Translator] Start remote mode -> %s", new_full_url.c_str());
		auto result_cb = [fd](const TranscriptionResult &r) {
			std::string texto = sanitize_text(r.text);
			if (!texto.empty()) {
				update_subtitle_source(fd, texto, r.is_final);
			}
		};
		auto status_cb = [fd](const std::string &status_text) {
			std::lock_guard<std::mutex> lock(fd->status_mutex);
			fd->connection_status = status_text;
		};
		fd->remote_client = new RemoteTranscriber(new_full_url, result_cb, status_cb);
	} else {
		blog(LOG_INFO, "[AI Translator] Start local mode (Whisper)");
		fd->processor = new audio_processor(fd->current_model_path);
	}

	// ── 4. Restart worker thread ─────────────────────────────────────────────
	fd->stop_worker.store(false);
	fd->worker_thread = std::thread(transcription_worker, fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// ai_filter_create
// ─────────────────────────────────────────────────────────────────────────────
static void *ai_filter_create(obs_data_t *settings, obs_source_t *source)
{
	(void)source;
	ai_filter_data *data = new ai_filter_data();
	data->resampler = nullptr;
	data->resampler_src_rate = 0;
	data->stop_worker.store(false);
	data->processor = nullptr;
	data->remote_client = nullptr;
	data->use_remote_transcription = false;
	data->ws_url = "";
	data->ws_token = "";

	// Read initial settings into data struct
	ai_filter_update(data, settings);
	
	// Clear timer logic is now in transcription_worker

	// Create appropriate backend based on initial configuration
	std::string full_url = build_full_ws_url(data->ws_url, data->ws_token, data->current_language, data->target_language);
	if (data->use_remote_transcription) {
		blog(LOG_INFO, "[AI Translator] Create with remote mode -> %s", full_url.empty() ? "(empty url)" : full_url.c_str());
		auto result_cb = [data](const TranscriptionResult &r) {
			std::string texto = sanitize_text(r.text);
			if (!texto.empty()) {
				blog(LOG_INFO, "[AI Translator] <- Remote (%s): %s",
				     r.is_final ? "FINAL" : "PARTIAL", texto.c_str());
				update_subtitle_source(data, texto, r.is_final);
			}
		};
		auto status_cb = [data](const std::string &status_text) {
			std::lock_guard<std::mutex> lock(data->status_mutex);
			data->connection_status = status_text;
		};
		data->remote_client = new RemoteTranscriber(full_url, result_cb, status_cb);
	} else {
		blog(LOG_INFO, "[AI Translator] Create with local mode (Whisper)");
		data->processor = new audio_processor(data->current_model_path);
	}

	// Start worker thread
	data->worker_thread = std::thread(transcription_worker, data);

	// Initialize Silero VAD
	struct whisper_vad_context_params vad_params = whisper_vad_default_context_params();
	char *vad_model_path = obs_module_file("models/silero_vad.bin");
	if (vad_model_path) {
		data->vad_ctx = whisper_vad_init_from_file_with_params(vad_model_path, vad_params);
		bfree(vad_model_path);
	} else {
		data->vad_ctx = nullptr;
	}

	data->vad.speech_frames.reserve(16000 * 30);
	data->vad.preroll.reserve(16000);
	data->vad.overlap_buffer.reserve(16000);

	// Overwrite any hallucinated text saved in OBS scene collection from previous sessions
	update_subtitle_source(data, "", true);

	return data;
}

// ─────────────────────────────────────────────────────────────────────────────
// ai_filter_destroy
//
// Cleanup sequence (CRITICAL to avoid use-after-free):
//   1. Stop worker thread
//   2. Destroy remote_client (waits for network thread termination)
//   3. Destroy processor
//   4. Free VAD and resampler
//   5. Delete main data structure
// ─────────────────────────────────────────────────────────────────────────────
static void ai_filter_destroy(void *data)
{
	ai_filter_data *fd = static_cast<ai_filter_data *>(data);

	// 1. Stop worker threads
	fd->stop_worker.store(true);
	fd->cv.notify_all();
	if (fd->worker_thread.joinable())
		fd->worker_thread.join();

		
	if (fd->subtitle_weak_ref) {
		obs_weak_source_release(fd->subtitle_weak_ref);
		fd->subtitle_weak_ref = nullptr;
	}

	// 2. Destroy RemoteTranscriber (destructor waits for network thread)
	//    MUST happen before freeing fd to prevent callbacks to dangling memory.
	if (fd->remote_client) {
		delete fd->remote_client;
		fd->remote_client = nullptr;
	}

	// Clean up segment queue and buffer pool
	{
		std::lock_guard<std::mutex> lock(fd->queue_mutex);
		while (!fd->segment_queue.empty()) {
			if (fd->segment_queue.front().audio) {
				delete fd->segment_queue.front().audio;
			}
			fd->segment_queue.pop();
		}
	}
	{
		std::lock_guard<std::mutex> lock(fd->pool_mutex);
		for (auto* buf : fd->buffer_pool) {
			delete buf;
		}
		fd->buffer_pool.clear();
	}

	// 3. Destroy Whisper processor
	if (fd->processor) {
		delete fd->processor;
		fd->processor = nullptr;
	}

	// 4. Free VAD context and resampler
	if (fd->vad_ctx)
		whisper_vad_free(fd->vad_ctx);
	if (fd->resampler) {
		audio_resampler_destroy(fd->resampler);
		fd->resampler = nullptr;
	}

	// 5. Delete main data structure
	delete fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// Default settings
// ─────────────────────────────────────────────────────────────────────────────
static void ai_filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "lang_in", "es");
	obs_data_set_default_string(settings, "lang_out", "en");
	obs_data_set_default_string(settings, "model_settings", "ggml-base.bin");
	obs_data_set_default_string(settings, "custom_model_path", "");
	obs_data_set_default_int(settings, "whisper_threads", 4);
	obs_data_set_default_bool(settings, "use_custom_model", false);
	obs_data_set_default_bool(settings, "processing_mode", false);
	obs_data_set_default_bool(settings, "local_translation", false);
	// Remote mode defaults
	obs_data_set_default_bool(settings, "use_remote_transcription", false);
	obs_data_set_default_string(settings, "ws_url", "");
	obs_data_set_default_string(settings, "ws_token", "");
	obs_data_set_default_int(settings, "auto_clear_seconds", 5);
}

// ─────────────────────────────────────────────────────────────────────────────
// get_audio_buffer — Fetch a buffer from the pool or create a new one
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<float>* get_audio_buffer(ai_filter_data *data)
{
	std::lock_guard<std::mutex> lock(data->pool_mutex);
	if (!data->buffer_pool.empty()) {
		auto* buf = data->buffer_pool.back();
		data->buffer_pool.pop_back();
		return buf;
	}
	auto* buf = new std::vector<float>();
	buf->reserve(16000 * 30);
	return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// _flush_segment — Finalize active speech segment and enqueue
// ─────────────────────────────────────────────────────────────────────────────
static void _flush_segment(ai_filter_data *filter_data)
{
	if (filter_data->vad.speech_ms >= MIN_SPEECH_MS) {
		AudioSegment seg;
		seg.audio = get_audio_buffer(filter_data);
		*seg.audio = filter_data->vad.speech_frames;
		seg.is_final = true;
		seg.sentence_id = filter_data->vad.sentence_id;

		{
			std::lock_guard<std::mutex> lock(filter_data->queue_mutex);
			filter_data->segment_queue.push(seg);
		}
		filter_data->cv.notify_one();

		filter_data->vad.sentence_id++;

		size_t overlap_samples = OVERLAP_MS * 16;
		if (filter_data->vad.speech_frames.size() > overlap_samples) {
			filter_data->vad.overlap_buffer.assign(
				filter_data->vad.speech_frames.end() - overlap_samples,
				filter_data->vad.speech_frames.end());
		} else {
			filter_data->vad.overlap_buffer = filter_data->vad.speech_frames;
		}
	}

	filter_data->vad.speaking = false;
	filter_data->vad.speech_frames.clear();
	filter_data->vad.speech_ms = 0;
	filter_data->vad.silence_ms = 0;
	filter_data->vad.last_partial_ms = 0;

	if (filter_data->vad_ctx)
		whisper_vad_reset_state(filter_data->vad_ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// ai_filter_audio — OBS audio callback (real-time audio thread)
//
// WARNING: NEVER perform network operations or blocking calls here.
//          Push data into queue (fast lock) and notify worker thread only.
// ─────────────────────────────────────────────────────────────────────────────
static struct obs_audio_data *ai_filter_audio(void *data, struct obs_audio_data *audio)
{
	ai_filter_data *filter_data = static_cast<ai_filter_data *>(data);

	// Process audio if any backend is active
	bool has_backend =
		(filter_data->processor != nullptr || filter_data->remote_client != nullptr);

	if (has_backend && audio->data[0]) {
		float *raw_audio = (float *)audio->data[0];
		size_t num_samples = audio->frames;
		struct obs_audio_info oai;

		if (obs_get_audio_info(&oai)) {
			std::vector<float> pcmf32;

			// Resample to 16kHz (if OBS input uses different rate, e.g., 48kHz)
			if (oai.samples_per_sec != 16000) {
				if (!filter_data->resampler ||
				    filter_data->resampler_src_rate != oai.samples_per_sec) {
					if (filter_data->resampler)
						audio_resampler_destroy(filter_data->resampler);

					struct resample_info src_info = {oai.samples_per_sec,
					                                 AUDIO_FORMAT_FLOAT, SPEAKERS_MONO};
					struct resample_info dst_info = {16000, AUDIO_FORMAT_FLOAT,
					                                 SPEAKERS_MONO};
					filter_data->resampler =
						audio_resampler_create(&dst_info, &src_info);
					filter_data->resampler_src_rate = oai.samples_per_sec;
				}

				if (filter_data->resampler) {
					const uint8_t *input_data[MAX_AV_PLANES] = {
						(const uint8_t *)raw_audio};
					uint8_t *output_data[MAX_AV_PLANES] = {nullptr};
					uint32_t out_frames = 0;
					uint64_t ts_offset = 0;

					if (audio_resampler_resample(filter_data->resampler, output_data,
					                             &out_frames, &ts_offset, input_data,
					                             (uint32_t)num_samples)) {
						if (out_frames > 0 && output_data[0]) {
							float *resampled = (float *)output_data[0];
							pcmf32.assign(resampled, resampled + out_frames);
						}
					}
				}
			} else {
				pcmf32.assign(raw_audio, raw_audio + num_samples);
			}

			if (!pcmf32.empty()) {
				// Remove DC offset
				float mean = 0.0f;
				for (float s : pcmf32)
					mean += s;
				mean /= pcmf32.size();
				for (float &s : pcmf32)
					s -= mean;

				// Calculate RMS for quick silence detection
				float sum = 0.0f;
				for (float s : pcmf32)
					sum += s * s;
				float rms = std::sqrt(sum / pcmf32.size());

				// VAD: detect speech
				bool is_speech = false;
				if (rms > SILENCE_RMS_THRESHOLD) {
					is_speech = whisper_vad_detect_speech_no_reset(
						filter_data->vad_ctx, pcmf32.data(), pcmf32.size());
				}

				size_t frame_ms = (pcmf32.size() * 1000) / 16000;

				if (is_speech) {
					if (!filter_data->vad.speaking) {
						filter_data->vad.speaking = true;
						filter_data->vad.speech_frames = filter_data->vad.overlap_buffer;
						filter_data->vad.speech_frames.insert(
							filter_data->vad.speech_frames.end(),
							filter_data->vad.preroll.begin(),
							filter_data->vad.preroll.end());
						filter_data->vad.overlap_buffer.clear();
						filter_data->vad.speech_ms =
							(filter_data->vad.speech_frames.size() * 1000) / 16000;
					}

					filter_data->vad.speech_frames.insert(
						filter_data->vad.speech_frames.end(), pcmf32.begin(),
						pcmf32.end());
					filter_data->vad.speech_ms += frame_ms;
					filter_data->vad.silence_ms = 0;

					// Send partial segment every second for low latency
					if (filter_data->vad.speech_ms - filter_data->vad.last_partial_ms >=
					    1000) {
						filter_data->vad.last_partial_ms = filter_data->vad.speech_ms;
						AudioSegment seg;
						seg.audio = get_audio_buffer(filter_data);
						*seg.audio = filter_data->vad.speech_frames;
						seg.is_final = false;
						seg.sentence_id = filter_data->vad.sentence_id;
						{
							std::lock_guard<std::mutex> lock(
								filter_data->queue_mutex);
							filter_data->segment_queue.push(seg);
						}
						filter_data->cv.notify_one();
					}
				} else {
					filter_data->vad.preroll.insert(filter_data->vad.preroll.end(),
					                                pcmf32.begin(), pcmf32.end());
					if (filter_data->vad.preroll.size() > (PREROLL_MS * 16)) {
						filter_data->vad.preroll.erase(
							filter_data->vad.preroll.begin(),
							filter_data->vad.preroll.begin() +
								(filter_data->vad.preroll.size() -
								 (PREROLL_MS * 16)));
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
							filter_data->vad.last_partial_ms =
								filter_data->vad.speech_ms;
							AudioSegment seg;
							seg.audio = get_audio_buffer(filter_data);
							*seg.audio = filter_data->vad.speech_frames;
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

				// Force segment flush if speech segment exceeds maximum length
				if (filter_data->vad.speaking &&
				    filter_data->vad.speech_ms >= MAX_SEGMENT_MS) {
					_flush_segment(filter_data);
				}
			}
		}
	}

	return audio;
}

// ─────────────────────────────────────────────────────────────────────────────
// Filter registration with OBS
// ─────────────────────────────────────────────────────────────────────────────
extern "C" struct obs_source_info get_ai_filter_info()
{
	struct obs_source_info info = {0};
	info.id = "ai_translation_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = ai_filter_get_name;
	info.get_defaults = ai_filter_get_defaults;
	info.update = ai_filter_update;
	info.create = ai_filter_create;
	info.destroy = ai_filter_destroy;
	info.get_properties = ai_filter_get_properties;
	info.filter_audio = ai_filter_audio;
	return info;
}