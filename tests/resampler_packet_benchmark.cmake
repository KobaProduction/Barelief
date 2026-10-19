if(NOT BUILD_DSP_ONLY AND NOT BUILD_PORTABLE_SDK)
    add_executable(resampler_packet_benchmark EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/examples/esp32_resampler_benchmark/host_sizes.cpp")
    target_compile_features(resampler_packet_benchmark PRIVATE cxx_std_20)
    target_link_libraries(resampler_packet_benchmark PRIVATE desktop_resampling)
    set_target_properties(resampler_packet_benchmark PROPERTIES AUTOMOC OFF)

    add_executable(resampler_quality_benchmark EXCLUDE_FROM_ALL resampler_quality_benchmark.cpp)
    target_compile_features(resampler_quality_benchmark PRIVATE cxx_std_20)
    target_link_libraries(resampler_quality_benchmark PRIVATE desktop_resampling)
    set_target_properties(resampler_quality_benchmark PROPERTIES AUTOMOC OFF)
endif()
