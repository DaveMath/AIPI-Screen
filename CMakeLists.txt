cmake_minimum_required(VERSION 3.16)

# Pin the target here so idf.py cannot silently produce an ESP32 image.
set(IDF_TARGET "esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(AIPI_Screen)

