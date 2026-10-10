# Cruller for the ESP32-S3 (ESP-IDF 6.1; ESP32-S3-DevKitC-1 N16R8): the common code in src/core plus
# this target (main/).
#   idf.py -C src/platform/esp32 -B build/esp32 build   (scripts/build.sh esp32 does it)
cmake_minimum_required(VERSION 3.22)

get_filename_component(CRULLER_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
include(${CRULLER_ROOT}/src/version.cmake)
set(CRULLER_VERSION "${CRULLER_VERSION_DEFAULT}" CACHE STRING "Firmware version (X.Y.Z)")
set(PROJECT_VER ${CRULLER_VERSION})
# The developer tools (src/platform/platform.h): the CI builds them into pre-releases and pull requests, not
# into releases.
option(CRULLER_DEBUG "The developer tools (src/platform/platform.h)" ON)
set(IDF_TARGET esp32s3)
# The generated sdkconfig goes to the build directory; sdkconfig.defaults here is the source.
set(SDKCONFIG ${CMAKE_BINARY_DIR}/sdkconfig)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cruller)
