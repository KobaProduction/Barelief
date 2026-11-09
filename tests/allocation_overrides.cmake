# Complete only the registered test-local allocation replacement families.
# The shared objects retain exceptions even for no-exception test consumers.
foreach(family IN ITEMS nothrow array)
    add_library(test_allocation_${family} OBJECT allocation_${family}_overrides.cpp)
    set_target_properties(test_allocation_${family} PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_STANDARD 20)
endforeach()
foreach(test IN ITEMS
    allocation_override_tests
    accumulator_runtime_tests
    array_math_runtime_tests
    aubio_reference_pipeline_tests
    beat_tracker_tests
    btrack_reference_pipeline_tests
    dataflow_runtime_tests
    desktop_history_demand_tests
    dsp_kernels_characterization_tests
    embedded_numeric_session_tests
    embedded_sdk_tests
    essentia_reference_pipeline_tests
    fft_size_benchmark
    fixed_window_tests
    fourier_runtime_tests
    graph_continuity_tests
    histogram_onset_source_tests
    index_extraction_tests
    madmom_precision_tests
    madmom_reference_pipeline_tests
    matrix_center_tests
    musical_features_tests
    musical_spectrogram_runtime_tests
    music_runtime_tests
    numeric_contract_tests
    numeric_control_codec_tests
    numeric_conversion_tests
    numeric_data_codec_tests
    numeric_frame_gather_tests
    numeric_operation_boundary_tests
    numeric_presentation_tests
    numeric_reduction_tests
    numeric_runtime_scenario_tests
    numeric_runtime_startup_tests
    numeric_values_runtime_tests
    onset_fusion_tests
    onset_strength_tests
    onset_tests
    portable_dsp_cadence_tests
    portable_dsp_tests
    precise_frontend_runtime_tests
    processing_control_tests
    real_fourier_provider_tests
    reference_array_geometry_tests
    reference_beat_pipeline_tests
    reference_precision_tests
    resampler_quality_benchmark
    resampling_modes_tests
    rhythm_kernel_tests
    routing_points_tests
    scalar_reduce_tests
    single_stream_runtime_tests
    spectral_descriptor_tests
    spectral_frontend_tests
    tempogram_runtime_tests
    temporal_fft_tests
    tempo_evidence_runtime_tests
    tempo_evidence_tests
    window_coefficients_runtime_tests
    wled_protocol_tests
)
    if(TARGET ${test})
        target_sources(${test} PRIVATE $<TARGET_OBJECTS:test_allocation_nothrow>)
    endif()
endforeach()
foreach(test IN ITEMS
    allocation_override_tests
    accumulator_runtime_tests
    array_math_runtime_tests
    btrack_reference_pipeline_tests
    embedded_numeric_session_tests
    embedded_sdk_tests
    essentia_reference_pipeline_tests
    fft_size_benchmark
    fourier_runtime_tests
    index_extraction_tests
    musical_features_tests
    musical_spectrogram_runtime_tests
    numeric_operation_boundary_tests
    onset_fusion_tests
    onset_tests
    portable_dsp_tests
    precise_frontend_runtime_tests
    real_fourier_provider_tests
    reference_array_geometry_tests
    reference_beat_pipeline_tests
    rhythm_kernel_tests
    window_coefficients_runtime_tests
)
    if(TARGET ${test})
        target_sources(${test} PRIVATE $<TARGET_OBJECTS:test_allocation_array>)
    endif()
endforeach()
