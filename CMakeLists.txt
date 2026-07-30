cmake_minimum_required(VERSION 3.28...3.30)
cmake_policy(SET CMP0177 NEW)

message(STATUS ">>> PROJECT NAME IS: ${CMAKE_PROJECT_NAME} <<<")
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
        src/audio_processor.h
        src/remote_transcriber.cpp
        src/remote_transcriber.h)

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

include(FetchContent)
# Allow direct FetchContent_Populate (avoid processing dependency CMakeLists.txt)
cmake_policy(SET CMP0169 OLD)

# === Asio standalone (without Boost) ===
FetchContent_Declare(
        asio
        URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(asio)
if(NOT asio_POPULATED)
    FetchContent_Populate(asio)
endif()

# === websocket++ (header-only, avoid processing legacy CMakeLists.txt) ===
FetchContent_Declare(
        websocketpp
        URL https://github.com/zaphoyd/websocketpp/archive/refs/tags/0.8.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(websocketpp)
if(NOT websocketpp_POPULATED)
    FetchContent_Populate(websocketpp)
    # Do not call add_subdirectory here: websocketpp is header-only
    # and its CMakeLists.txt is incompatible with modern CMake
endif()

# Add websocketpp and Asio include directories to plugin
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${websocketpp_SOURCE_DIR}
        ${asio_SOURCE_DIR}/asio/include
)
# Required definitions: use standalone Asio (without Boost) and C++11 STL
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
)

# === libopus (audio codec for compressed voice stream to server) ===
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::OPUS)
message(STATUS "Opus found: ${OPUS_VERSION}")

# === OpenSSL (required for wss:// TLS WebSocket connections) ===
find_package(OpenSSL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")


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

# Set base OBS plugin installation directory depending on OS
if(WIN32)
    set(OBS_PLUGIN_INSTALL_DIR "$ENV{APPDATA}/obs-studio/plugins/${CMAKE_PROJECT_NAME}")
elseif(APPLE)
    set(OBS_PLUGIN_INSTALL_DIR "$ENV{HOME}/Library/Application Support/obs-studio/plugins/${CMAKE_PROJECT_NAME}")
else()
    set(OBS_PLUGIN_INSTALL_DIR "$ENV{HOME}/.config/obs-studio/plugins/${CMAKE_PROJECT_NAME}")
endif()

# Automate plugin copy to OBS on post-build
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        # Create directories safely across platforms
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_INSTALL_DIR}/bin/64bit/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_INSTALL_DIR}/data/models/"

        # Copy compiled binary
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" "${OBS_PLUGIN_INSTALL_DIR}/bin/64bit/"

        # Copy models directory recursively
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/models" "${OBS_PLUGIN_INSTALL_DIR}/data/models"

        COMMENT "Install plugin and models into OBS automatically for current system..."
)

# Create executable for UI testing
#add_executable(test_ui test_ui.cpp ui/setup.cpp ui/setup.h ui/setup.ui
#)

# Link with Qt and enable UI compilation
#target_link_libraries(test_ui PRIVATE Qt6::Widgets)
#set_target_properties(test_ui PROPERTIES AUTOMOC ON AUTOUIC ON)

# whisper.cpp
# Force compiler optimizations (Release mode)
set(CMAKE_BUILD_TYPE Release)
# Force advanced CPU math optimizations (AVX2/FMA)
set(GGML_AVX ON CACHE BOOL "" FORCE)
set(GGML_AVX2 ON CACHE BOOL "" FORCE)
set(GGML_FMA ON CACHE BOOL "" FORCE)
set(GGML_F16C ON CACHE BOOL "" FORCE)

# Enable Vulkan GPU acceleration support
set(GGML_VULKAN ON CACHE BOOL "" FORCE)

# Disable strict GCC 16 errors to prevent whisper.cpp build failure
add_compile_options(-Wno-error=enum-conversion -Wno-error=nonnull)

set(WHISPER_ALL_WARNINGS OFF) # Avoid build warnings
add_subdirectory(vendor/whisper.cpp)
# Extra safety patch for GCC:
target_compile_options(whisper PRIVATE -Wno-error=format-truncation)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE whisper)

# WhisperCPP Models
set(WHISPER_MODELS
        "models/ggml-tiny.bin"
        "models/ggml-base.bin"
        "models/ggml-small.bin"
        "models/silero_vad.bin")
# Install model files
install(FILES ${WHISPER_MODELS}
        DESTINATION "${OBS_DATA_DEST}/${OBS_PLUGIN_NAME}/models")