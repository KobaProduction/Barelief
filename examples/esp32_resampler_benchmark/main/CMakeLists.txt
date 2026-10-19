set(resampler_benchmark_main "main.cpp")
if(RESAMPLER_BENCH_PACKET_SWEEP AND RESAMPLER_BENCH_PROBE_ONLY)
    message(FATAL_ERROR "Select packet sweep or dot probe, not both")
endif()
if(RESAMPLER_BENCH_PACKET_SWEEP)
    set(resampler_benchmark_main "main_sizes.cpp")
elseif(RESAMPLER_BENCH_PROBE_ONLY)
    set(resampler_benchmark_main "main_probe.cpp")
endif()
idf_component_register(SRCS "${resampler_benchmark_main}"
    "../../../libs/dsp_kernels/src/prepared_resampler.cpp"
    "../../../adapters/desktop/resampling/speex_backend.cpp"
    "../../../adapters/desktop/resampling/speex_bridge.c"
    INCLUDE_DIRS "../../../libs/dsp_kernels/include" "../../../adapters/desktop/resampling"
    REQUIRES esp_timer heap)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
