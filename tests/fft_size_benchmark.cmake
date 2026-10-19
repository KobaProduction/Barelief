# Isolated host measurement profile: same production FFT sources, 16384
# capacity, independent vendor globals. Application and firmware keep 4096.
set(fft_sweep_root "${CMAKE_SOURCE_DIR}/libs/dsp_kernels")
set(fft_vendor_root "${fft_sweep_root}/vendor/esp_dsp")
add_library(fft_size_benchmark_vendor STATIC EXCLUDE_FROM_ALL
    "${fft_vendor_root}/common/misc/dsps_pwroftwo.cpp"
    "${fft_vendor_root}/fft/float/dsps_fft2r_fc32_ansi.c"
    "${fft_vendor_root}/fft/float/dsps_fft4r_fc32_ansi.c"
    "${fft_vendor_root}/fft/float/dsps_fft2r_bitrev_tables_fc32.c"
    "${fft_vendor_root}/fft/float/dsps_fft4r_bitrev_tables_fc32.c")
target_include_directories(fft_size_benchmark_vendor PUBLIC
    "${fft_vendor_root}/common/include" "${fft_vendor_root}/common/include_sim"
    "${fft_vendor_root}/fft/include")
target_compile_definitions(fft_size_benchmark_vendor PRIVATE CONFIG_DSP_MAX_FFT_SIZE=16384)
add_executable(fft_size_benchmark EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/fft_size_benchmark.cpp"
    "${fft_sweep_root}/src/dsp.cpp" "${fft_sweep_root}/src/rfft_split.cpp"
    "${fft_sweep_root}/src/backend_tables.cpp" "${fft_sweep_root}/src/backend_ansi.cpp")
target_include_directories(fft_size_benchmark PRIVATE "${fft_sweep_root}/include")
target_compile_definitions(fft_size_benchmark PRIVATE PORTABLE_DSP_MAX_FFT_SIZE=16384)
target_compile_features(fft_size_benchmark PRIVATE cxx_std_20)
set_target_properties(fft_size_benchmark fft_size_benchmark_vendor PROPERTIES AUTOMOC OFF)
target_link_libraries(fft_size_benchmark PRIVATE signal_contracts fft_size_benchmark_vendor)
