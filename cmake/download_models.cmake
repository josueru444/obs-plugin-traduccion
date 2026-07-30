# Automatic download of Whisper and Silero VAD model files if missing
set(MODELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/models")
file(MAKE_DIRECTORY "${MODELS_DIR}")

set(MODEL_URL_ggml-tiny.bin "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin")
set(MODEL_URL_ggml-base.bin "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin")
set(MODEL_URL_ggml-small.bin "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin")
set(MODEL_URL_silero_vad.bin "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v5.1.2.bin")

set(REQUIRED_MODELS
    "ggml-tiny.bin"
    "ggml-base.bin"
    "ggml-small.bin"
    "silero_vad.bin"
)

foreach(MODEL_FILE ${REQUIRED_MODELS})
    set(MODEL_PATH "${MODELS_DIR}/${MODEL_FILE}")
    if(NOT EXISTS "${MODEL_PATH}")
        set(MODEL_URL "${MODEL_URL_${MODEL_FILE}}")
        message(STATUS "Downloading missing model: ${MODEL_FILE}...")
        file(DOWNLOAD
            "${MODEL_URL}"
            "${MODEL_PATH}"
            SHOW_PROGRESS
            STATUS DOWNLOAD_STATUS
            INACTIVITY_TIMEOUT 30
            TIMEOUT 600
        )
        list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
        list(GET DOWNLOAD_STATUS 1 ERROR_MSG)
        if(NOT STATUS_CODE EQUAL 0)
            message(WARNING "Failed to download model ${MODEL_FILE}: ${ERROR_MSG}")
            if(EXISTS "${MODEL_PATH}")
                file(REMOVE "${MODEL_PATH}")
            endif()
        else()
            message(STATUS "Successfully downloaded ${MODEL_FILE}")
        endif()
    else()
        message(STATUS "Model ${MODEL_FILE} already present.")
    endif()
endforeach()
