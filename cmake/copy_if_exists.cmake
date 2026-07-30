# copy_if_exists.cmake
# Copies SRC directory to DST only if SRC exists.
# Usage: cmake -DSRC=<source> -DDST=<dest> -P copy_if_exists.cmake
if(EXISTS "${SRC}")
    file(COPY "${SRC}/" DESTINATION "${DST}")
else()
    message(STATUS "Models directory '${SRC}' not found — skipping copy.")
endif()
