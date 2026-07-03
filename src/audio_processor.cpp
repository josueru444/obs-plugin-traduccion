//
// Created by pce on 7/2/26.
//

#include "audio_processor.h"
#include <iostream>
#include <thread>
#include <algorithm>
#include <obs-module.h>

// Constructor
audio_processor::audio_processor(const std::string &model_path)
{
	struct whisper_context_params cparams = whisper_context_default_params();
	ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);

	if (ctx == nullptr) {
		std::cerr << "Error whisper_init_from_file_with_params()" << std::endl;
	} else {

		std::cout << "Audio whisper loaded successfully" << std::endl;
	}
}
// Destructor
audio_processor::~audio_processor()
{
	if (ctx != nullptr) {
		whisper_free(ctx);
		ctx = nullptr;
	}
}

// Processing Function
std::string audio_processor::process_audio(const std::vector<float> &pcmf32, const std::string &language,
					   bool translate, const std::string &initial_prompt)
{

	// if model fail loading, do nothing
	if (ctx == nullptr || pcmf32.empty()) {
		return "";
	}

	whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	wparams.print_progress = false;
	wparams.print_special = false;
	wparams.print_realtime = false;
	wparams.print_timestamps = false;

	wparams.n_threads = std::min(8u, std::thread::hardware_concurrency());
	wparams.single_segment = true;
	wparams.no_context = true;
	wparams.temperature_inc = 0.0f;

	wparams.language = language.c_str(); // it will use the form option
	wparams.translate = translate;
	if (!initial_prompt.empty()) {
		wparams.initial_prompt = initial_prompt.c_str();
	}

	// .data() obtiene el puntero crudo del vector, y .size() la cantidad de muestras
	if (whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size()) != 0) {
		std::cerr << "[Audio processor] Error whisper_full()]" << std::endl;
		return "";
	}

	// get text
	std::string final_result = "";
	int n_segments = whisper_full_n_segments(ctx);

	for (int i = 0; i < n_segments; i++) {
		const char *segment_text = whisper_full_get_segment_text(ctx, i);
		final_result += segment_text;
	}
	return final_result;
}
