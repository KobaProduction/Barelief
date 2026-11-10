# Explicit source manifest: compile the exact public desktop/host SDK sources
# with the target toolchain. Never link a desktop archive into ESP firmware.
get_filename_component(PORTABLE_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(PORTABLE_SDK_SOURCES
    libs/signal_contracts/src/frame.cpp libs/signal_contracts/src/buffer_pool.cpp
    libs/signal_contracts/src/data_contract.cpp libs/signal_contracts/src/data_frame.cpp
    libs/signal_contracts/src/numeric_frame_gather.cpp
    libs/bdsp_protocol/src/byte_stream.cpp
    libs/bdsp_protocol/src/stream_fragments.cpp
    libs/bdsp_protocol/src/numeric_data_message.cpp libs/bdsp_protocol/src/numeric_data_codec.cpp
    libs/bdsp_protocol/src/numeric_control_message.cpp libs/bdsp_protocol/src/numeric_control_codec.cpp
    libs/bdsp_protocol/generated/numeric_control.pb.c
    libs/bdsp_protocol/vendor/tinycbor/src/cborencoder.c
    libs/bdsp_protocol/vendor/tinycbor/src/cborencoder_close_container_checked.c
    libs/bdsp_protocol/vendor/tinycbor/src/cborencoder_float.c
    libs/bdsp_protocol/vendor/tinycbor/src/cborparser.c
    libs/bdsp_protocol/vendor/tinycbor/src/cborparser_float.c
    libs/bdsp_protocol/vendor/tinycbor/src/cborerrorstrings.c
    libs/bdsp_protocol/vendor/tinycbor/src/cborvalidation.c
    libs/bdsp_protocol/vendor/nanopb/pb_common.c
    libs/bdsp_protocol/vendor/nanopb/pb_encode.c
    libs/bdsp_protocol/vendor/nanopb/pb_decode.c
    libs/wled_protocol/src/wled_packet.cpp
    domain/src/graph_definition.cpp domain/src/graph_validation.cpp
    domain/src/graph/processing_parameters.cpp domain/src/graph/window_parameters.cpp domain/src/graph/node_factory.cpp
    domain/src/graph/parameter_registry.cpp
    domain/src/graph/node_validation.cpp
    domain/src/numeric/parameter_descriptors.cpp
    domain/src/signal/parameter_descriptors.cpp
    domain/src/music/parameter_descriptors.cpp
    domain/src/graph/routing_parameter_descriptors.cpp
    domain/src/graph/adapter_parameter_descriptors.cpp
    domain/src/graph/view_parameter_descriptors.cpp
    domain/src/graph/graph_admission.cpp
    domain/src/graph/clock_provenance.cpp
    domain/src/graph/owned_footprint.cpp
    domain/src/graph/configuration_bindings.cpp
    domain/src/graph/preparation_vector_values.cpp
    domain/src/numeric/contract_snapshot.cpp domain/src/numeric/array_generator_contract.cpp
     domain/src/numeric/interval_operation_contracts.cpp
    domain/src/numeric/common_operation_contracts.cpp
    domain/src/numeric/sequence_operation_contracts.cpp domain/src/numeric/spectral_sequence_parameters.cpp domain/src/numeric/reference_operation_contracts.cpp
    domain/src/numeric/matrix_contracts.cpp domain/src/signal/fourier_contracts.cpp
    domain/src/signal/parameter_contracts.cpp domain/src/music/spectral_features_contract.cpp
    domain/src/signal/window_coefficients.cpp
    domain/src/music/frequency_normalizer_contract.cpp
    domain/src/music/rhythm_contract.cpp
    domain/src/music/tempo_evidence_contract.cpp
    domain/src/music/onset_strength_contract.cpp
    domain/src/music/spectral_descriptor_contract.cpp
    domain/src/signal/power_to_db_contract.cpp
    domain/src/signal/mel_contract.cpp
    domain/src/graph/processing_contracts.cpp
    domain/src/numeric_parameter_bindings.cpp
    domain/src/music/musical_spectrogram_contract.cpp
    domain/src/numeric/array_product_contract.cpp
    domain/src/numeric/array_concat_contract.cpp
    domain/src/numeric/array_extraction.cpp
    application/src/graph_lifecycle.cpp application/src/processing_control.cpp
    application/src/frequency_profile_application.cpp
    adapters/control/src/numeric_processing_message_mapper.cpp
    adapters/control/src/numeric_control_session.cpp
    adapters/project_storage/src/numeric_array_codec.cpp
    libs/dataflow_runtime/src/runtime.cpp
    libs/dataflow_runtime/src/numeric_operation.cpp
    libs/dataflow_runtime/src/numeric_operation_binding.cpp
    libs/dataflow_runtime/src/numeric_operation_invocation.cpp
    libs/dataflow_runtime/src/numeric_operation_requirements.cpp
    libs/dataflow_runtime/src/numeric_operation_axis_requirements.cpp
    libs/dataflow_runtime/src/numeric_operation_metadata_values.cpp
    libs/dataflow_runtime/src/numeric_operation_coordinate_routes.cpp
    libs/dataflow_runtime/src/numeric_extension_invocation.cpp
    libs/dataflow_runtime/src/numeric_extension_preparation.cpp
    libs/dataflow_runtime/src/numeric_extension_resources.cpp
    libs/dataflow_runtime/src/numeric_extension_storage.cpp
    libs/dataflow_runtime/src/numeric_extension_latch.cpp
    libs/dataflow_runtime/src/numeric_extension_join.cpp
    libs/dataflow_runtime/src/numeric_extension_publication.cpp
    libs/dataflow_runtime/src/numeric_extension_process.cpp
    libs/dataflow_runtime/src/runtime_context.cpp
    libs/dataflow_runtime/src/music_execution.cpp
    libs/dataflow_runtime/src/numeric_operation_preparation.cpp
    libs/dataflow_runtime/src/signal_operation_preparation.cpp
    libs/dataflow_runtime/src/signal_feature_preparation.cpp
    libs/dataflow_runtime/src/music_operation_preparation.cpp
    libs/dataflow_runtime/src/numeric_array_execution.cpp
    libs/dataflow_runtime/src/array_extraction.cpp
    libs/dataflow_runtime/src/signal_operation_execution.cpp
    libs/dataflow_runtime/src/music_operation_execution.cpp
    libs/dataflow_runtime/src/rhythm_execution.cpp
    libs/dataflow_runtime/src/tempo_evidence_execution.cpp
    libs/dataflow_runtime/src/onset_strength_execution.cpp
    libs/dataflow_runtime/src/spectral_descriptor_execution.cpp
    libs/dataflow_runtime/src/spectral_frontend_execution.cpp
    libs/dataflow_runtime/src/prepared_operation_process.cpp
    libs/dataflow_runtime/src/pcm_packet_queue.cpp
    libs/dataflow_runtime/src/runtime_queries.cpp
    libs/dataflow_runtime/src/graph_preparation.cpp
    libs/dataflow_runtime/src/graph_node_preparation.cpp
    libs/dataflow_runtime/src/graph_continuity_preparation.cpp
    libs/dataflow_runtime/src/value_execution.cpp
    libs/dataflow_runtime/src/ingress_acceptance.cpp
    libs/dataflow_runtime/src/clock_mix_execution.cpp
    libs/dataflow_runtime/src/history_execution.cpp
    libs/dataflow_runtime/src/conversion_execution.cpp
    libs/dataflow_runtime/src/resampler_execution.cpp
    libs/dataflow_runtime/src/window_execution.cpp
    libs/dataflow_runtime/src/capability_admission.cpp
    libs/dataflow_runtime/src/buffer_preparation.cpp
    libs/dataflow_runtime/src/node_state.cpp
    libs/dataflow_runtime/src/signal_time.cpp
    libs/dataflow_runtime/src/frame_access.cpp
    libs/dataflow_runtime/src/numeric_execution.cpp
    libs/dataflow_runtime/src/matrix_execution.cpp
    libs/dataflow_runtime/src/transform_execution.cpp
    libs/dataflow_runtime/src/frame_buffer_execution.cpp
    libs/dataflow_runtime/src/mixer_execution.cpp
    libs/dataflow_runtime/src/clock_execution.cpp
    libs/dsp_kernels/src/dsp.cpp libs/dsp_kernels/src/rfft_split.cpp libs/dsp_kernels/src/backend_tables.cpp libs/dsp_kernels/src/scalar_reduce.cpp
    libs/dsp_kernels/src/prepared_resampler.cpp libs/dsp_kernels/src/recursive_filter.cpp libs/dsp_kernels/src/spectral_autocorrelation.cpp libs/dsp_kernels/src/blocked_fourier.cpp libs/dsp_kernels/src/value_histogram.cpp libs/dsp_kernels/src/envelope_follower.cpp libs/dsp_kernels/src/sequence_statistics.cpp libs/dsp_kernels/src/model_prediction_selection.cpp libs/dsp_kernels/src/tempo_probability_histogram.cpp libs/dsp_kernels/src/beat_feature_synchronization.cpp
    libs/dsp_kernels/src/sparse_markov.cpp libs/dsp_kernels/src/metrical_state_space.cpp libs/dsp_kernels/src/metrical_transitions.cpp
    libs/dsp_kernels/src/metrical_observations.cpp libs/dsp_kernels/src/bar_state_decoder.cpp libs/dsp_kernels/src/downbeat_state_decoder.cpp
    libs/dsp_kernels/src/matrix_factorization.cpp libs/dsp_kernels/src/gaussian_mixture.cpp libs/dsp_kernels/src/metrical_events.cpp libs/dsp_kernels/src/pattern_state_decoder.cpp
    libs/dsp_kernels/src/rectangular_filter_bank.cpp
      libs/dsp_kernels/src/state_beat_sequence.cpp
    libs/dsp_kernels/src/frame_sampling.cpp libs/dsp_kernels/src/spectral_frame_recipe.cpp libs/dsp_kernels/src/spectral_frame_storage.cpp libs/dsp_kernels/src/interpolated_peaks.cpp libs/dsp_kernels/src/spectral_novelty.cpp libs/dsp_kernels/src/spectral_novelty_storage.cpp
    libs/dsp_kernels/src/discrete_triangle.cpp libs/dsp_kernels/src/sinc_resampling_window.cpp libs/dsp_kernels/src/resampling_window.cpp
    libs/dsp_kernels/src/feature_reduction.cpp

    libs/dsp_kernels/src/musical_features.cpp
    libs/dsp_kernels/src/frequency_profile.cpp libs/dsp_kernels/src/frequency_curve.cpp libs/dsp_kernels/src/frequency_curve_fit.cpp
    libs/dsp_kernels/src/frequency_measurements.cpp libs/dsp_kernels/src/frequency_measurement_grid.cpp
    libs/dsp_kernels/src/onsets.cpp libs/dsp_kernels/src/onset_fusion.cpp
    libs/dsp_kernels/src/correlation.cpp
    libs/dsp_kernels/src/spectral_alignment.cpp libs/dsp_kernels/src/comb_filter_bank.cpp
    libs/dsp_kernels/src/interval_histogram.cpp libs/dsp_kernels/src/convolution.cpp
    libs/dsp_kernels/src/histogram_tempo_peaks.cpp libs/dsp_kernels/src/interval_beat_search.cpp
    libs/dsp_kernels/src/interval_sequence_decoder.cpp libs/dsp_kernels/src/state_interval_histogram.cpp
    libs/dsp_kernels/src/local_interval_beat_search.cpp libs/dsp_kernels/src/interval_sequence_search.cpp
    libs/dsp_kernels/src/tempo_evidence.cpp
    libs/dsp_kernels/src/onset_strength.cpp libs/dsp_kernels/src/onset_lag.cpp
    libs/dsp_kernels/src/beat_tracker.cpp
    libs/dsp_kernels/src/fourier_backend.cpp libs/dsp_kernels/src/fourier_transform.cpp libs/dsp_kernels/src/fourier_storage.cpp libs/dsp_kernels/src/temporal_window.cpp
    libs/dsp_kernels/src/tempogram.cpp libs/dsp_kernels/src/tempo_estimator.cpp
    libs/dsp_kernels/src/reference_fourier.cpp libs/dsp_kernels/src/reference_autocorrelation.cpp
    libs/dsp_kernels/src/onset_peaks.cpp libs/dsp_kernels/src/overlap_add.cpp

    libs/dsp_kernels/src/beat_state_decoder.cpp
    libs/dsp_kernels/src/log_frequency_filter_bank.cpp libs/dsp_kernels/src/complex_flux.cpp
    libs/dsp_kernels/src/energy_novelty.cpp
    libs/dsp_kernels/src/complex_values.cpp libs/dsp_kernels/src/spectral_descriptors.cpp libs/dsp_kernels/src/log_compression.cpp
    libs/dsp_kernels/src/activation_peaks.cpp  libs/dsp_kernels/src/curve_interpolation.cpp
    libs/dsp_kernels/src/sample_rotation.cpp
    libs/dsp_kernels/src/mel_filter_bank.cpp libs/dsp_kernels/src/filter_projection.cpp libs/dsp_kernels/src/power_to_db.cpp
    libs/dsp_kernels/src/rhythm/analyzer.cpp libs/dsp_kernels/src/rhythm/math.cpp
    libs/dsp_kernels/src/rhythm/observer.cpp libs/dsp_kernels/src/rhythm/tracking.cpp
    libs/dsp_kernels/src/rhythm/autocorrelation.cpp libs/dsp_kernels/src/rhythm/hierarchy.cpp
    libs/dsp_kernels/src/rhythm/ranking.cpp libs/dsp_kernels/src/rhythm/decision.cpp
    libs/dsp_kernels/src/rhythm/publication.cpp libs/dsp_kernels/src/rhythm/packing.cpp
    libs/dsp_kernels/src/kernel_support.cpp libs/dsp_kernels/src/numeric_convert.cpp
    libs/dsp_kernels/src/numeric_reduce.cpp
    libs/embedded_support/src/profile.cpp libs/embedded_support/src/session.cpp
    libs/embedded_support/src/synthetic_pipeline.cpp libs/embedded_support/src/persistence.cpp)
set(PORTABLE_SDK_INCLUDES
    libs/signal_contracts/include libs/bdsp_protocol/include libs/wled_protocol/include
    libs/bdsp_protocol/vendor/tinycbor/src libs/bdsp_protocol/vendor/tinycbor/include
    libs/bdsp_protocol/vendor/nanopb
    libs/bdsp_protocol/generated
    domain/include application/include libs/dataflow_runtime/include
    libs/dsp_kernels/include libs/embedded_support/include adapters/control/include adapters/project_storage/include)
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
