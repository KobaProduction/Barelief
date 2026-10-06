# Explicit cross-build only. Dependencies come from the caller's environment.
set(ESP32_SDK_BUILD_DIR "${CMAKE_BINARY_DIR}/esp32-sdk" CACHE PATH "Separate cross-build directory")
set(ESP32_SDK_IDF_PATH "$ENV{IDF_PATH}" CACHE PATH "Existing ESP-IDF source tree")
set(ESP32_TOOLCHAIN_DIR "$ENV{ESP32_TOOLCHAIN_DIR}" CACHE PATH "Existing Xtensa toolchain root (contains bin)")
set(ESP32_SDK_FFT_CAPACITY 4096 CACHE STRING "ESP32 compiled FFT capacity")
option(ESP32_SDK_ESP_DSP "Optimized ESP-DSP in the cross SDK" ON)
set(ESP32_SDK_PYTHON "" CACHE FILEPATH "Optional existing IDF Python interpreter")
set(ESP32_IDF_TOOLS_PATH "$ENV{IDF_TOOLS_PATH}" CACHE PATH "Existing IDF tool/constraint cache")
set(esp32_python_arg)
if(ESP32_SDK_PYTHON)
    set(esp32_python_arg "-DPYTHON=${ESP32_SDK_PYTHON}")
endif()
set(esp32_env_arg)
if(ESP32_IDF_TOOLS_PATH)
    set(esp32_env_arg "IDF_TOOLS_PATH=${ESP32_IDF_TOOLS_PATH}")
endif()
if(ESP32_SDK_PYTHON)
    get_filename_component(esp32_python_bin "${ESP32_SDK_PYTHON}" DIRECTORY)
    get_filename_component(esp32_python_env "${esp32_python_bin}" DIRECTORY)
    if(EXISTS "${esp32_python_env}/pyvenv.cfg")
        list(APPEND esp32_env_arg "IDF_PYTHON_ENV_PATH=${esp32_python_env}")
    endif()
endif()
add_custom_target(esp32_sdk
    COMMAND "${CMAKE_COMMAND}" -E env ${esp32_env_arg}
        "${CMAKE_COMMAND}" -S "${CMAKE_SOURCE_DIR}/adapters/esp_idf/library"
        -B "${ESP32_SDK_BUILD_DIR}" -G Ninja
        "-DESP_IDF_SOURCE_DIR=${ESP32_SDK_IDF_PATH}"
        "-DESP32_TOOLCHAIN_DIR=${ESP32_TOOLCHAIN_DIR}"
        "-DPORTABLE_SDK_MAX_FFT_SIZE=${ESP32_SDK_FFT_CAPACITY}"
        "-DPORTABLE_SDK_ESP_DSP=${ESP32_SDK_ESP_DSP}"
        "-DBDSP_SOURCE_DIR=${bdsp_SOURCE_DIR}"
        "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        ${esp32_python_arg}
    COMMAND "${CMAKE_COMMAND}" --build "${ESP32_SDK_BUILD_DIR}" --target esp32_sdk
    USES_TERMINAL VERBATIM)
