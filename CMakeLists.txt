cmake_minimum_required(VERSION 3.28...3.30)
cmake_policy(SET CMP0177 NEW)

message(STATUS ">>> EL NOMBRE DEL PROYECTO ES: ${CMAKE_PROJECT_NAME} <<<")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE
        src/ai_audio_filter.cpp
        src/ai_audio_filter.h
        #ui/setup.cpp
        #ui/setup.h
        src/ai_subtitle_source.cpp
        src/ai_subtitle_source.h
        src/audio_processor.cpp
        src/audio_processor.h)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if (ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif ()

if (ENABLE_QT)
    find_package(Qt6 COMPONENTS Widgets Core)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
    target_compile_options(
            ${CMAKE_PROJECT_NAME}
            PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
    )
    set_target_properties(
            ${CMAKE_PROJECT_NAME}
            PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
    )
endif ()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

find_package(Qt6 REQUIRED COMPONENTS Widgets)
target_link_libraries(${CMAKE_PROJECT_NAME}
        PRIVATE
        OBS::libobs
        Qt6::Widgets
)
#target_sources(${CMAKE_PROJECT_NAME} PRIVATE
#        ui/setup.ui
#        ui/setup.cpp
#        ui/setup.h
#        ui/ui_setup.h
#)


set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON)

# Directorio base de instalación de plugins para OBS según el sistema operativo
if(WIN32)
    set(OBS_PLUGIN_INSTALL_DIR "$ENV{APPDATA}/obs-studio/plugins/${CMAKE_PROJECT_NAME}")
elseif(APPLE)
    set(OBS_PLUGIN_INSTALL_DIR "$ENV{HOME}/Library/Application Support/obs-studio/plugins/${CMAKE_PROJECT_NAME}")
else()
    set(OBS_PLUGIN_INSTALL_DIR "$ENV{HOME}/.config/obs-studio/plugins/${CMAKE_PROJECT_NAME}")
endif()

# Automatizar la copia del plugin a OBS cada vez que compilamos
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        # Crear directorios de forma segura y multiplataforma
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_INSTALL_DIR}/bin/64bit/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_INSTALL_DIR}/data/models/"
        
        # Copiar el binario compilado
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" "${OBS_PLUGIN_INSTALL_DIR}/bin/64bit/"
        
        # Copiar la carpeta de modelos de forma recursiva
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/models" "${OBS_PLUGIN_INSTALL_DIR}/data/models"
        
        COMMENT "Instalando plugin y modelos en OBS de forma automatica para el sistema actual..."
)

# Crear un programa ejecutable solo para probar la UI
#add_executable(test_ui test_ui.cpp ui/setup.cpp ui/setup.h ui/setup.ui
#)

# Enlazarlo con Qt y activar la compilación de UI
#target_link_libraries(test_ui PRIVATE Qt6::Widgets)
#set_target_properties(test_ui PROPERTIES AUTOMOC ON AUTOUIC ON)

#whisper cpp
# ¡CRÍTICO PARA LA VELOCIDAD! Forzar optimizaciones del compilador (Release mode).
set(CMAKE_BUILD_TYPE Release)
# Forzar optimizaciones matemáticas avanzadas del procesador (AVX2/FMA)
set(GGML_AVX ON CACHE BOOL "" FORCE)
set(GGML_AVX2 ON CACHE BOOL "" FORCE)
set(GGML_FMA ON CACHE BOOL "" FORCE)
set(GGML_F16C ON CACHE BOOL "" FORCE)

# Habilitar soporte Vulkan para aceleración por GPU (compatible con AMD)
set(GGML_VULKAN ON CACHE BOOL "" FORCE)

# Deshabilitar errores estrictos de GCC 16 para evitar que falle whisper.cpp
add_compile_options(-Wno-error=enum-conversion -Wno-error=nonnull)

set(WHISPER_ALL_WARNINGS OFF) # Evita que advertencias tontas de Linux rompan la compilación
add_subdirectory(vendor/whisper.cpp)
# Parche de seguridad extra para GCC:
target_compile_options(whisper PRIVATE -Wno-error=format-truncation)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE whisper)

# WhisperCPP Models
set(WHISPER_MODELS
        "models/ggml-tiny.bin"
        "models/ggml-base.bin"
        "models/ggml-small.bin"
        "models/silero_vad.bin")
# install the models
install(FILES ${WHISPER_MODELS}
        DESTINATION "${OBS_DATA_DEST}/${OBS_PLUGIN_NAME}/models")