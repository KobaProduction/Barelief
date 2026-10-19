add_library(dataflow_runtime STATIC
    src/runtime.cpp
    src/runtime_context.cpp
    src/runtime_queries.cpp
    src/graph_preparation.cpp
    src/graph_node_preparation.cpp
    src/graph_continuity_preparation.cpp
    src/value_execution.cpp
    src/ingress_acceptance.cpp
    src/clock_mix_execution.cpp
    src/history_execution.cpp
    src/conversion_execution.cpp
    src/resampler_execution.cpp
    src/window_execution.cpp
    src/capability_admission.cpp
    src/buffer_preparation.cpp
    src/node_state.cpp
    src/signal_time.cpp
    src/frame_access.cpp
    src/numeric_execution.cpp
    src/matrix_execution.cpp
    src/music_execution.cpp
    src/numeric_operation_preparation.cpp
    src/signal_operation_preparation.cpp
    src/signal_feature_preparation.cpp
    src/music_operation_preparation.cpp
    src/numeric_array_execution.cpp
    src/signal_operation_execution.cpp
    src/music_operation_execution.cpp
    src/prepared_operation_process.cpp
    src/pcm_packet_queue.cpp
    src/transform_execution.cpp
    src/mixer_execution.cpp
    src/clock_execution.cpp)
target_include_directories(dataflow_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(dataflow_runtime PUBLIC cxx_std_20)
target_link_libraries(dataflow_runtime PUBLIC graph_application dsp_kernels wled_protocol)
install(TARGETS dataflow_runtime EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
