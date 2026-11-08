add_library(dsp_graph_adapters STATIC
    src/factory.cpp src/operation_support.cpp src/operation_constraints.cpp src/absolute_value.cpp
    src/onset_bindings.cpp src/tempo_bindings.cpp src/beat_pulse_bindings.cpp
    src/histogram_bindings.cpp src/state_bindings.cpp
    src/model_bindings.cpp src/spectral_sequence_settings.cpp src/spectral_sequence_bindings.cpp src/spectral_composition.cpp
    src/common_axis.cpp src/common_operations.cpp src/common_support.cpp src/common_values.cpp
    src/common_filters.cpp src/common_sequences.cpp src/feature_aggregate.cpp src/temporal_frame_sequence.cpp src/profile.cpp)
target_include_directories(dsp_graph_adapters PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(dsp_graph_adapters PUBLIC cxx_std_20)
target_link_libraries(dsp_graph_adapters PUBLIC dataflow_runtime graph_domain dsp_kernels signal_contracts)
