# Optional AGPL composition. Main application and SDK do not link/install it.
add_library(rhythm_graph_adapters STATIC
    src/adapter_operation.cpp
    src/btrack_composition.cpp
    src/btrack_operation_plan.cpp
    src/btrack_operation_prepare.cpp
    src/btrack_operation_process.cpp
    src/btrack_operation_outputs.cpp
    src/essentia_composition.cpp
    src/essentia_frequency_grid.cpp
    src/essentia_operation_plan.cpp
    src/essentia_operation_process.cpp
    src/essentia_operation_assessment.cpp

    src/batch_composition.cpp
    src/batch_operation_assessment.cpp
    src/batch_operation_io.cpp
    src/batch_operation_plan.cpp
    src/batch_operation_shapes.cpp
    src/buffered_composition.cpp
    src/buffered_operation_assessment.cpp
    src/buffered_operation_inputs.cpp
    src/buffered_operation_plan.cpp
    src/buffered_operation_prepare.cpp
    src/buffered_operation_process.cpp
    src/composition.cpp
    src/contrast_composition.cpp
    src/contrast_operation_assessment.cpp
    src/contrast_operation_inputs.cpp
    src/contrast_operation_outputs.cpp
    src/contrast_operation_plan.cpp
    src/contrast_operation_prepare.cpp
    src/contrast_operation_process.cpp
    src/descriptor_composition.cpp
    src/descriptor_operation_assessment.cpp
    src/descriptor_operation_bands.cpp
    src/descriptor_operation_inputs.cpp
    src/descriptor_operation_plan.cpp
    src/descriptor_operation_prepare.cpp
    src/descriptor_operation_process.cpp
    src/event_composition.cpp
    src/event_operation_assessment.cpp
    src/event_operation_inputs.cpp
    src/event_operation_plan.cpp
    src/event_operation_prepare.cpp
    src/event_operation_process.cpp
    src/factory.cpp
    src/frame_operation_io.cpp
    src/global_tracking_operations.cpp
    src/histogram_batch_operations.cpp
    src/interval_operation_assessment.cpp
    src/interval_operation_prepare.cpp
    src/interval_operation_process.cpp
    src/legacy_tracking_operations.cpp
    src/loop_operations.cpp
    src/loudness_composition.cpp
    src/loudness_energy_operations.cpp
    src/loudness_matrix_operations.cpp
    src/loudness_operation_assessment.cpp
    src/loudness_operation_io.cpp
    src/loudness_operation_plan.cpp
    src/novelty_batch_operations.cpp
    src/operation_io.cpp
    src/parameter_values.cpp
    src/pcm_stage_apply.cpp
    src/pcm_stage_assessment.cpp
    src/pcm_stage_buffers.cpp
    src/pcm_stage_composition.cpp
    src/pcm_stage_context_outputs.cpp
    src/pcm_stage_controls.cpp
    src/pcm_stage_coordinates.cpp
    src/pcm_stage_geometry.cpp
    src/pcm_stage_inputs.cpp
    src/pcm_stage_plan.cpp
    src/pcm_stage_ports.cpp
    src/pcm_stage_prepare.cpp
    src/pcm_stage_process.cpp
    src/pcm_stage_recipe_outputs.cpp
    src/pcm_stage_settings.cpp
    src/pcm_stage_spectrum_outputs.cpp
    src/period_tracking_operations.cpp
    src/predictive_composition.cpp
    src/predictive_operation_contracts.cpp
    src/predictive_operation_outputs.cpp
    src/predictive_operation_plan.cpp
    src/predictive_operation_prepare.cpp
    src/predictive_operation_controls.cpp
    src/predictive_operation_process.cpp
    src/profile.cpp
    src/pulse_batch_operations.cpp
    src/sequence_source_assessment.cpp
    src/sequence_source_composition.cpp
    src/sequence_source_geometry.cpp
    src/sequence_source_inputs.cpp
    src/sequence_source_io.cpp
    src/sequence_source_onset_outputs.cpp
    src/sequence_source_onsets.cpp
    src/sequence_source_outputs.cpp
    src/sequence_source_plan.cpp
    src/sequence_source_prepare.cpp
    src/sequence_source_process.cpp
    src/sequence_source_settings.cpp
    src/sequence_source_storage.cpp
    src/sequence_source_stream.cpp
    src/sequence_source_stream_outputs.cpp
    src/spectral_composition.cpp
    src/spectral_operation_assessment.cpp
    src/spectral_operation_inputs.cpp
    src/spectral_operation_plan.cpp
    src/spectral_operation_prepare.cpp
    src/spectral_operation_process.cpp
    src/tracking_composition.cpp
    src/tracking_operation_assessment.cpp
    src/tracking_operation_plan.cpp
    src/tracking_operation_shapes.cpp

)
target_include_directories(rhythm_graph_adapters PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(rhythm_graph_adapters PUBLIC cxx_std_20)
target_link_libraries(rhythm_graph_adapters PUBLIC dataflow_runtime graph_domain
    rhythm_reference_ports rhythm_probabilistic_reference dsp_graph_adapters)
set_property(TARGET rhythm_graph_adapters PROPERTY SPDX_LICENSE_IDENTIFIER "AGPL-3.0-or-later")
add_executable(rhythm_operations_composition EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/examples/rhythm_operations/profile_composition.cpp"
    "${PROJECT_SOURCE_DIR}/examples/rhythm_operations/spectral_diagnostics.cpp")
target_link_libraries(rhythm_operations_composition PRIVATE rhythm_graph_adapters)
target_compile_features(rhythm_operations_composition PRIVATE cxx_std_20)
