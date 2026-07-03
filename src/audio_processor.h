//
// Created by pce on 7/2/26.
//

#ifndef PLUGINTEMPLATE_FOR_OBS_AUDIO_PROCESSOR_H
#define PLUGINTEMPLATE_FOR_OBS_AUDIO_PROCESSOR_H

#include "whisper.h"
#include <string>
#include <vector>

class audio_processor
{
      public:
    audio_processor(const std::string &model_path);
    ~audio_processor();

	std::string process_audio(const std::vector<float> &pcmf32, const std::string &language = "auto", bool translate = false, const std::string &initial_prompt = "");

      private:
    struct whisper_context *ctx = nullptr;
    // (Opcional) Puedes guardar parámetros de whisper aquí para no crearlos en cada llamada
    // struct whisper_full_params wparams;
};

#endif  //PLUGINTEMPLATE_FOR_OBS_AUDIO_PROCESSOR_H
