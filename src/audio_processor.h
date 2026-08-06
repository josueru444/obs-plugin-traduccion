#ifndef PLUGINTEMPLATE_FOR_OBS_AUDIO_PROCESSOR_H
#define PLUGINTEMPLATE_FOR_OBS_AUDIO_PROCESSOR_H

#include "whisper.h"
#include <string>
#include <vector>

class audio_processor
{
      public:
    audio_processor(const std::string &model_path, bool use_gpu = false);
    ~audio_processor();

	std::string process_audio(const std::vector<float> &pcmf32, const std::string &language = "auto", bool translate = false, const std::string &initial_prompt = "", int n_threads = 4);

      private:
    struct whisper_context *ctx = nullptr;
    // Store Whisper parameters to avoid re-creation per call
    // struct whisper_full_params wparams;
};

#endif  //PLUGINTEMPLATE_FOR_OBS_AUDIO_PROCESSOR_H
