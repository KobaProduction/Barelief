set(GMF_FFT_SOURCE_DIR "" CACHE PATH "Explicit pinned gmf_fft source for optional comparison benchmark")
if(GMF_FFT_SOURCE_DIR)
    include("${CMAKE_SOURCE_DIR}/examples/gmf_fft_benchmark/vendor_sources.cmake")
    add_executable(gmf_fft_benchmark EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/examples/gmf_fft_benchmark/common/benchmark.cpp" ${gmf_probe_sources})
    target_include_directories(gmf_fft_benchmark PRIVATE
        "${CMAKE_SOURCE_DIR}/examples/gmf_fft_benchmark/common"
        "${GMF_FFT_SOURCE_DIR}/include" "${GMF_FFT_SOURCE_DIR}/src/common")
    target_compile_features(gmf_fft_benchmark PRIVATE cxx_std_20)
    target_link_libraries(gmf_fft_benchmark PRIVATE dsp_kernels)
    set_target_properties(gmf_fft_benchmark PROPERTIES AUTOMOC OFF)
endif()
