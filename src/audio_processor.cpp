#include "audio_processor.h"
#include <iostream>
#include <thread>
#include <algorithm>
#include <obs-module.h>

audio_processor::audio_processor(const std::string &model_path, bool use_gpu)
{
	if (model_path.empty()) {
		blog(LOG_ERROR, "[AI Translator] Whisper model path is empty! Local transcription disabled.");
		ctx = nullptr;
		return;
	}

	struct whisper_context_params cparams = whisper_context_default_params();
	cparams.use_gpu = use_gpu;
	ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);

	if (ctx == nullptr) {
		blog(LOG_ERROR, "[AI Translator] Error loading whisper model from path: '%s'. Make sure the ggml model file exists!", model_path.c_str());
	} else {
		blog(LOG_INFO, "[AI Translator] Whisper model loaded successfully: '%s' (GPU: %s)", model_path.c_str(), use_gpu ? "ENABLED" : "DISABLED");
	}
}

audio_processor::~audio_processor()
{
	if (ctx != nullptr) {
		whisper_free(ctx);
		ctx = nullptr;
	}
}

std::string audio_processor::process_audio(const std::vector<float> &pcmf32, const std::string &language,
					   bool translate, const std::string &initial_prompt, int n_threads)
{
	if (ctx == nullptr) {
		blog(LOG_WARNING, "[AI Translator] Local inference skipped: Whisper context is NULL (model not loaded).");
		return "";
	}
	if (pcmf32.empty()) {
		return "";
	}

	(void)initial_prompt;

	whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	wparams.print_progress = false;
	wparams.print_special = false;
	wparams.print_realtime = false;
	wparams.print_timestamps = false;

	wparams.n_threads = (n_threads > 0) ? n_threads : (int)std::min(4u, std::thread::hardware_concurrency());
	wparams.single_segment = true;
	wparams.no_context = true;
	wparams.no_timestamps = true;
	wparams.temperature = 0.0f;
	wparams.temperature_inc = 0.0f;

	wparams.language = language.c_str();
	wparams.translate = translate;

	// Configure anti-hallucination thresholds
	wparams.suppress_blank = true;
	wparams.suppress_nst = true;
	wparams.entropy_thold = 2.40f;
	wparams.logprob_thold = -0.50f;
	wparams.no_speech_thold = 0.6f;

	if (whisper_full(ctx, wparams, pcmf32.data(), (int)pcmf32.size()) != 0) {
		blog(LOG_WARNING, "[AI Translator] whisper_full failed to process audio segment.");
		return "";
	}

	std::string final_result = "";
	int n_segments = whisper_full_n_segments(ctx);

	for (int i = 0; i < n_segments; i++) {
		float no_speech_prob = whisper_full_get_segment_no_speech_prob(ctx, i);
		if (no_speech_prob > 0.6f) {
			continue;
		}
		const char *segment_text = whisper_full_get_segment_text(ctx, i);
		final_result += segment_text;
	}

	return final_result;
}
