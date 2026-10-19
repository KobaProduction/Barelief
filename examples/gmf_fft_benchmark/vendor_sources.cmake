# An explicitly supplied, pinned upstream checkout; no network activity in CMake.
if(NOT EXISTS "${GMF_FFT_SOURCE_DIR}/src/common/fft_power2_q15.c")
    message(FATAL_ERROR "Provide GMF_FFT_SOURCE_DIR (esp-gmf 255fb531ddc97ff5e1e113dad97c88f70d6297be/packages/gmf_fft)")
endif()
file(READ "${CMAKE_CURRENT_LIST_DIR}/vendor.lock.json" gmf_lock)
string(JSON gmf_file_count LENGTH "${gmf_lock}" files)
math(EXPR gmf_last_file "${gmf_file_count} - 1")
foreach(gmf_index RANGE 0 ${gmf_last_file})
    string(JSON gmf_path GET "${gmf_lock}" files ${gmf_index} path)
    string(JSON gmf_expected GET "${gmf_lock}" files ${gmf_index} sha256)
    file(SHA256 "${GMF_FFT_SOURCE_DIR}/${gmf_path}" gmf_actual)
    if(NOT gmf_actual STREQUAL gmf_expected)
        message(FATAL_ERROR "GMF source differs from pinned experiment: ${gmf_path}")
    endif()
endforeach()
set(gmf_probe_sources
    "${CMAKE_CURRENT_LIST_DIR}/common/gmf_probe.c"
    "${CMAKE_CURRENT_LIST_DIR}/common/gmf_portable_optimized.c"
    "${CMAKE_CURRENT_LIST_DIR}/common/gmf_radix4.c"
    "${CMAKE_CURRENT_LIST_DIR}/common/gmf_radix4_simd.c"
    "${CMAKE_CURRENT_LIST_DIR}/common/gmf_radix4_model.c"
    "${CMAKE_CURRENT_LIST_DIR}/common/gmf_hp4_pack_simd.c"
    "${CMAKE_CURRENT_LIST_DIR}/common/gmf_radix4_profile.c"
    "${CMAKE_CURRENT_LIST_DIR}/common/gmf_hp_optimized.c"
    "${CMAKE_CURRENT_LIST_DIR}/common/gmf_hp_pack_optimized.c"
    "${CMAKE_CURRENT_LIST_DIR}/common/gmf_hp4_pack.c"
    "${CMAKE_CURRENT_LIST_DIR}/common/probe_heap.c")
if(IDF_TARGET STREQUAL "esp32s3")
    if(GMF_S3_VECTOR_PERMUTATION)
        list(APPEND gmf_probe_sources "${CMAKE_CURRENT_LIST_DIR}/common/gmf_permutation_aes3.S")
    endif()
    if(GMF_R4_S3_SIMD)
        list(APPEND gmf_probe_sources "${CMAKE_CURRENT_LIST_DIR}/common/gmf_radix4_aes3.S")
    endif()
    list(APPEND gmf_probe_sources
        "${GMF_FFT_SOURCE_DIR}/src/esp32s3/fft_bitrev_s16.c"
        "${GMF_FFT_SOURCE_DIR}/src/esp32s3/fft_pie_radix2_dit_s16.S"
        "${GMF_FFT_SOURCE_DIR}/src/esp32s3/fft_pie_radix2_hp_s16.S")
else()
    list(APPEND gmf_probe_sources
        "${GMF_FFT_SOURCE_DIR}/src/esp_c/gmf_fft_radix2_dit_s16.c"
        "${GMF_FFT_SOURCE_DIR}/src/esp_c/gmf_fft_radix2_hp_s16.c"
        "${GMF_FFT_SOURCE_DIR}/src/esp_c/fft_hp_pack.c"
        "${GMF_FFT_SOURCE_DIR}/src/esp_c/fft_hp_scale_s16.c")
endif()
