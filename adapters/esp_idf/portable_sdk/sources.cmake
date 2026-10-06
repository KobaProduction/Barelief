# Explicit source manifest: compile the exact public desktop/host SDK sources
# with the target toolchain. Never link a desktop archive into ESP firmware.
get_filename_component(PORTABLE_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(PORTABLE_SDK_SOURCES
    libs/signal_contracts/src/frame.cpp libs/signal_contracts/src/buffer_pool.cpp
    libs/bdsp_protocol/src/stream_codec.cpp libs/bdsp_protocol/src/byte_stream.cpp
    libs/bdsp_protocol/src/stream_fragments.cpp
    domain/src/graph_definition.cpp domain/src/graph_validation.cpp
    application/src/graph_lifecycle.cpp libs/dataflow_runtime/src/runtime.cpp
    libs/dsp_kernels/src/dsp.cpp libs/dsp_kernels/src/backend_tables.cpp
    libs/embedded_support/src/profile.cpp libs/embedded_support/src/session.cpp
    libs/embedded_support/src/synthetic_pipeline.cpp)
set(PORTABLE_SDK_INCLUDES
    libs/signal_contracts/include libs/bdsp_protocol/include
    domain/include application/include libs/dataflow_runtime/include
    libs/dsp_kernels/include libs/embedded_support/include)
if(PORTABLE_SDK_ESP_DSP)
    list(APPEND PORTABLE_SDK_SOURCES libs/dsp_kernels/src/backend_esp_dsp.cpp)
else()
    list(APPEND PORTABLE_SDK_SOURCES libs/dsp_kernels/src/backend_ansi.cpp)
    set(ansi_root libs/dsp_kernels/vendor/esp_dsp)
    foreach(source IN ITEMS common/misc/dsps_pwroftwo.cpp
        fft/float/dsps_fft2r_fc32_ansi.c fft/float/dsps_fft4r_fc32_ansi.c
        fft/float/dsps_fft2r_bitrev_tables_fc32.c fft/float/dsps_fft4r_bitrev_tables_fc32.c)
        list(APPEND PORTABLE_SDK_SOURCES "${ansi_root}/${source}")
    endforeach()
    # Use ESP-IDF's esp_err/esp_attr/log headers, not the host simulation headers.
    list(APPEND PORTABLE_SDK_INCLUDES "${ansi_root}/common/include" "${ansi_root}/fft/include")
endif()
list(TRANSFORM PORTABLE_SDK_SOURCES PREPEND "${PORTABLE_SDK_ROOT}/")
list(TRANSFORM PORTABLE_SDK_INCLUDES PREPEND "${PORTABLE_SDK_ROOT}/")
