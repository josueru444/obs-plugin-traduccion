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

# Windows: link Winsock2 required by standalone Asio
if(WIN32)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 mswsock)
endif()

# === libopus (audio codec for compressed voice stream to server) ===
if(WIN32)
    # Windows obs-deps does not include standalone Opus, build it via FetchContent
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        opus
        URL https://github.com/xiph/opus/archive/refs/tags/v1.5.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(opus)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE opus)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${opus_SOURCE_DIR}/include)
elseif(APPLE)
    find_package(Opus REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Opus::opus)
else()
    # Linux: use pkg-config as Opus is typically installed system-wide
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::OPUS)
endif()

# === OpenSSL (required for wss:// TLS WebSocket connections) ===
find_package(OpenSSL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")

if(WIN32)
    # FindOpenSSL often creates UNKNOWN targets which breaks IMPORTED_RUNTIME_ARTIFACTS
    # We manually find the .dlls in the adjacent bin/ directory
    if(OPENSSL_SSL_LIBRARY)
        get_filename_component(_ssl_lib_dir "${OPENSSL_SSL_LIBRARY}" DIRECTORY)
        get_filename_component(_ssl_root_dir "${_ssl_lib_dir}" DIRECTORY)
        file(GLOB OPENSSL_DLLS "${_ssl_root_dir}/bin/libssl*.dll" "${_ssl_root_dir}/bin/libcrypto*.dll")
        if(OPENSSL_DLLS)
            install(FILES ${OPENSSL_DLLS} DESTINATION bin/64bit)
        endif()
    endif()
endif()


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
if(WIN32)
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
            # Create directories safely across platforms
            COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_INSTALL_DIR}/bin/64bit/"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_INSTALL_DIR}/data/models/"

            # Copy compiled binary
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" "${OBS_PLUGIN_INSTALL_DIR}/bin/64bit/"

            # Copy models directory recursively only if it exists (models are optional in CI)
            COMMAND ${CMAKE_COMMAND}
                    -DSRC="${CMAKE_CURRENT_SOURCE_DIR}/models"
                    -DDST="${OBS_PLUGIN_INSTALL_DIR}/data/models"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_exists.cmake"

            COMMENT "Install plugin and models into OBS automatically for current system..."
    )
    if(OPENSSL_DLLS)
        add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENSSL_DLLS} "${OBS_PLUGIN_INSTALL_DIR}/bin/64bit/"
        )
    endif()
else()
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
            # Create directories safely across platforms
            COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_INSTALL_DIR}/bin/64bit/"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_INSTALL_DIR}/data/models/"

            # Copy compiled binary
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" "${OBS_PLUGIN_INSTALL_DIR}/bin/64bit/"

            # Copy models directory recursively only if it exists (models are optional in CI)
            COMMAND ${CMAKE_COMMAND}
                    -DSRC="${CMAKE_CURRENT_SOURCE_DIR}/models"
                    -DDST="${OBS_PLUGIN_INSTALL_DIR}/data/models"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_exists.cmake"

            COMMENT "Install plugin and models into OBS automatically for current system..."
    )
endif()

# Create executable for UI testing
#add_executable(test_ui test_ui.cpp ui/setup.cpp ui/setup.h ui/setup.ui
#)

# Link with Qt and enable UI compilation
#target_link_libraries(test_ui PRIVATE Qt6::Widgets)
#set_target_properties(test_ui PROPERTIES AUTOMOC ON AUTOUIC ON)

# whisper.cpp
# For single-config generators, default to Release for whisper performance
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# CPU math optimizations (safe defaults for x86_64)
set(GGML_AVX ON CACHE BOOL "" FORCE)
set(GGML_AVX2 ON CACHE BOOL "" FORCE)
set(GGML_FMA ON CACHE BOOL "" FORCE)
set(GGML_F16C ON CACHE BOOL "" FORCE)

# Vulkan GPU acceleration support (optional, only if SDK + shader compiler is available)
find_package(Vulkan QUIET COMPONENTS glslc)
if(NOT Vulkan_FOUND)
    find_package(Vulkan QUIET COMPONENTS glslangValidator)
endif()

if(Vulkan_FOUND)
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)
    message(STATUS "Vulkan SDK with shader compiler found — enabling GPU acceleration for whisper.cpp")
else()
    set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
    message(STATUS "Vulkan shader compiler not found — whisper.cpp will use CPU only")
endif()

# Disable strict GCC/Clang errors to prevent whisper.cpp build failure
if(NOT MSVC)
    add_compile_options(-Wno-error=enum-conversion -Wno-error=nonnull)
endif()

set(WHISPER_ALL_WARNINGS OFF) # Avoid build warnings

# Build whisper.cpp statically to prevent __create_def crash on Windows and missing DLLs
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF CACHE BOOL "Export all symbols" FORCE)

# Ensure position-independent code (-fPIC) for static libs linked into shared module (.so)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Disable OpenMP to prevent dynamic dependency on vcomp140.dll on Windows (causes Error 126)
set(GGML_OPENMP OFF CACHE BOOL "Disable OpenMP" FORCE)

# Disable warnings as errors for 3rd party code
set(SAVE_WARNINGS_AS_ERRORS ${CMAKE_COMPILE_WARNING_AS_ERROR})
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)

add_subdirectory(vendor/whisper.cpp SYSTEM)

set(CMAKE_COMPILE_WARNING_AS_ERROR ${SAVE_WARNINGS_AS_ERRORS})

# Disable WINDOWS_EXPORT_ALL_SYMBOLS on created targets if any
if(TARGET whisper)
    set_target_properties(whisper PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()
if(TARGET ggml)
    set_target_properties(ggml PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()
if(TARGET ggml-cpu)
    set_target_properties(ggml-cpu PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()

# Extra safety patch for GCC/Clang:
if(NOT MSVC)
    target_compile_options(whisper PRIVATE -Wno-error=format-truncation)
endif()

# Disable all warnings and warnings-as-errors for whisper.cpp and main target under MSVC
if(MSVC)
    set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /WX-)
    target_compile_options(whisper PRIVATE /W0 /WX-)
    if(TARGET ggml)
        target_compile_options(ggml PRIVATE /W0 /WX-)
    endif()
    if(TARGET ggml-base)
        target_compile_options(ggml-base PRIVATE /W0 /WX-)
    endif()
    if(TARGET ggml-cpu)
        target_compile_options(ggml-cpu PRIVATE /W0 /WX-)
    endif()
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE whisper)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE vendor/whisper.cpp/include)

# Auto-download missing models (tiny, base, small, silero_vad)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/download_models.cmake")

# Install model files into plugin data folder
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/models/"
        DESTINATION "${CMAKE_PROJECT_NAME}/data/models"
        OPTIONAL)