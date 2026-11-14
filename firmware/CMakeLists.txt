# The Luanette firmware (ESP-IDF 6.1; PlatformIO wraps the same project: platformio.ini).
#   idf.py set-target esp32s3
#   idf.py build flash
cmake_minimum_required(VERSION 3.16)

# The version: PROJECT_VER from the environment (the release workflow sets it from the tag), else this.
if(DEFINED ENV{PROJECT_VER})
    set(PROJECT_VER "$ENV{PROJECT_VER}")
elseif(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "dev")  # a local build (the release sets the tag's version): board.js lets "dev" take anything
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(luanette)
