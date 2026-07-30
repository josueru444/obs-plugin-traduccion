# FindOpus.cmake
# Locate the Opus audio codec library (provided by obs-deps prebuilt packages or system)
#
# This module defines:
#   Opus_FOUND        - True if Opus was found
#   Opus_INCLUDE_DIRS - Include directories for Opus
#   Opus_LIBRARIES    - Libraries to link against
#   Opus::opus        - Imported target

include(FindPackageHandleStandardArgs)

file(GLOB _obs_deps_dirs "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-*")

find_path(
  Opus_INCLUDE_DIR
  NAMES opus/opus.h opus.h
  HINTS ${CMAKE_PREFIX_PATH} ${_obs_deps_dirs}
  PATH_SUFFIXES include include/opus
)

find_library(
  Opus_LIBRARY
  NAMES opus libopus opus-0 libopus-0 opusfile libopusfile
  HINTS ${CMAKE_PREFIX_PATH} ${_obs_deps_dirs}
  PATH_SUFFIXES lib lib64 lib/x64 bin bin/64bit
)

find_package_handle_standard_args(
  Opus
  REQUIRED_VARS Opus_LIBRARY Opus_INCLUDE_DIR
)

if(Opus_FOUND AND NOT TARGET Opus::opus)
  add_library(Opus::opus UNKNOWN IMPORTED)
  set_target_properties(
    Opus::opus
    PROPERTIES
      IMPORTED_LOCATION "${Opus_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Opus_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(Opus_INCLUDE_DIR Opus_LIBRARY)
