cmake_minimum_required(VERSION 3.28...3.30)

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
        ui/setup.cpp
        ui/setup.h
        src/ai_subtitle_source.cpp
        src/ai_subtitle_source.h
        src/audio_processor.cpp
        src/audio_processor.h)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
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
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

find_package(Qt6 REQUIRED COMPONENTS Widgets)
target_link_libraries(${CMAKE_PROJECT_NAME}
        PRIVATE
        OBS::libobs
        Qt6::Widgets
)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ui/setup.ui
        ui/setup.cpp
        ui/setup.h
        ui/ui_setup.h
)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON)

# Automatizar la copia del plugin a OBS cada vez que compilamos
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND mkdir -p $ENV{HOME}/.config/obs-studio/plugins/${CMAKE_PROJECT_NAME}/bin/64bit/
    COMMAND cp $<TARGET_FILE:${CMAKE_PROJECT_NAME}> $ENV{HOME}/.config/obs-studio/plugins/${CMAKE_PROJECT_NAME}/bin/64bit/
    COMMENT "✨ Instalando plugin en OBS de forma automatica..."
)

# Crear un programa ejecutable solo para probar la UI
add_executable(test_ui test_ui.cpp ui/setup.cpp ui/setup.h ui/setup.ui
)

# Enlazarlo con Qt y activar la compilación de UI
target_link_libraries(test_ui PRIVATE Qt6::Widgets)
set_target_properties(test_ui PROPERTIES AUTOMOC ON AUTOUIC ON)

#whisper cpp
# ¡CRÍTICO PARA LA VELOCIDAD! Forzar optimizaciones del compilador (Release mode).
set(CMAKE_BUILD_TYPE Release)
# Forzar optimizaciones matemáticas avanzadas del procesador (AVX2/FMA)
set(GGML_AVX ON CACHE BOOL "" FORCE)
set(GGML_AVX2 ON CACHE BOOL "" FORCE)
set(GGML_FMA ON CACHE BOOL "" FORCE)
set(GGML_F16C ON CACHE BOOL "" FORCE)

set(WHISPER_ALL_WARNINGS OFF) # Evita que advertencias tontas de Linux rompan la compilación
add_subdirectory(vendor/whisper.cpp)
# Parche de seguridad extra para GCC:
target_compile_options(whisper PRIVATE -Wno-error=format-truncation) 
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE whisper)