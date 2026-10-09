cmake_minimum_required(VERSION 3.24)
option(PORTABLE_SDK_ESP_DSP "Use the existing optimized ESP-DSP component" ON)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../adapters/esp_idf/bdsp"
    "${CMAKE_CURRENT_LIST_DIR}/../../adapters/esp_idf/portable_sdk"
    "${CMAKE_CURRENT_LIST_DIR}/../../adapters/esp_idf/stream_adapter"
    "${CMAKE_CURRENT_LIST_DIR}/../../adapters/esp_idf/persistence")
if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "ESP-IDF is not installed/configured. Configure the host test separately; do not install from this project.")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_streamer)
