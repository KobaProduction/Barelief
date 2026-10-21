cmake_minimum_required(VERSION 3.24)
project(gmf_sweep_host LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(fft_root "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(fft_dsp "${fft_root}/libs/dsp_kernels")
add_subdirectory("${fft_root}/libs/signal_contracts" signal_contracts)
add_subdirectory("${fft_dsp}/vendor/esp_dsp" dsp_vendor)
target_compile_definitions(dsp PRIVATE CONFIG_DSP_MAX_FFT_SIZE=16384)
add_library(dsp_kernels STATIC
    "${fft_dsp}/src/dsp.cpp" "${fft_dsp}/src/rfft_split.cpp"
    "${fft_dsp}/src/backend_tables.cpp" "${fft_dsp}/src/backend_ansi.cpp"
    "${fft_dsp}/src/scalar_reduce.cpp" "${fft_dsp}/src/kernel_support.cpp")
target_include_directories(dsp_kernels PUBLIC "${fft_dsp}/include"
    "${fft_root}/libs/signal_contracts/include")
target_compile_definitions(dsp_kernels PUBLIC PORTABLE_DSP_MAX_FFT_SIZE=16384)
target_link_libraries(dsp_kernels PUBLIC dsp signal_contracts)
include("${CMAKE_CURRENT_LIST_DIR}/../vendor_sources.cmake")
add_executable(gmf_candidate_check ../common/candidate_check.cpp ${gmf_probe_sources})
target_compile_definitions(gmf_candidate_check PRIVATE GMF_TEST_MATH=1)
target_include_directories(gmf_candidate_check PRIVATE ../common
    "${GMF_FFT_SOURCE_DIR}/include" "${GMF_FFT_SOURCE_DIR}/src/common")
add_executable(gmf_hp_check ../common/hp_check.cpp ${gmf_probe_sources})
target_include_directories(gmf_hp_check PRIVATE ../common
    "${GMF_FFT_SOURCE_DIR}/include" "${GMF_FFT_SOURCE_DIR}/src/common")
add_executable(gmf_sweep ../common/sweep.cpp ${gmf_probe_sources})
add_executable(gmf_r4_simd_check ../common/simd_check.cpp ${gmf_probe_sources})
target_include_directories(gmf_r4_simd_check PRIVATE ../common
    "${GMF_FFT_SOURCE_DIR}/include" "${GMF_FFT_SOURCE_DIR}/src/common")
target_include_directories(gmf_sweep PRIVATE ../common
    "${GMF_FFT_SOURCE_DIR}/include" "${GMF_FFT_SOURCE_DIR}/src/common"
    "${fft_dsp}/src")
target_link_libraries(gmf_sweep PRIVATE dsp_kernels)
foreach(fft_test rfft_tests complex_fft_tests)
    add_executable(${fft_test} "${fft_root}/tests/${fft_test}.cpp")
    target_link_libraries(${fft_test} PRIVATE dsp_kernels)
endforeach()
option(GMF_UBSAN "Check adapted arithmetic with undefined behavior sanitizer" OFF)
option(GMF_TRAP_OVERFLOW "Check signed arithmetic using GCC overflow traps" OFF)
option(GMF_R4_HOST_MODEL "Route experimental R4 through PIE semantic model, not a PC benchmark" OFF)
if(GMF_R4_HOST_MODEL)
    foreach(gmf_target gmf_candidate_check gmf_hp_check gmf_sweep gmf_r4_simd_check)
        target_compile_definitions(${gmf_target} PRIVATE GMF_R4_ROUTE_MODEL=1)
    endforeach()
endif()
if(GMF_TRAP_OVERFLOW)
    target_compile_options(gmf_candidate_check PRIVATE -ftrapv)
    target_compile_options(gmf_hp_check PRIVATE -ftrapv)
    target_compile_options(gmf_r4_simd_check PRIVATE -ftrapv)
endif()
if(GMF_UBSAN)
    target_compile_options(gmf_candidate_check PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
    target_link_options(gmf_candidate_check PRIVATE -fsanitize=undefined)
endif()

option(GMF_HEADER_AUDIT "Compile the experiment's C-compatible headers as a C++ consumer" OFF)
if(GMF_HEADER_AUDIT)
    add_library(quality_gmf_headers OBJECT "${fft_root}/tools/quality/gmf_headers.cpp")
    target_include_directories(quality_gmf_headers PRIVATE
        $<TARGET_PROPERTY:gmf_candidate_check,INCLUDE_DIRECTORIES>)
    target_compile_definitions(quality_gmf_headers PRIVATE
        $<TARGET_PROPERTY:gmf_candidate_check,COMPILE_DEFINITIONS>)
    target_compile_features(quality_gmf_headers PRIVATE cxx_std_20)
endif()
