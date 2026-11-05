# Opt-in offline measurement composition. Uses the production graph and decoder;
# no audio device, GUI, alternate executor or DSP implementation.
add_library(rhythm_replay_support STATIC
    src/replay.cpp src/observations.cpp src/output.cpp src/tempograms.cpp src/tempo_candidates.cpp src/reference_recording.cpp)
set_target_properties(rhythm_replay_support PROPERTIES AUTOMOC OFF)
target_include_directories(rhythm_replay_support PUBLIC include PRIVATE src)
target_compile_features(rhythm_replay_support PUBLIC cxx_std_20)
target_link_libraries(rhythm_replay_support PUBLIC Qt6::Core PRIVATE
    desktop_analysis desktop_source_formats desktop_numeric_defaults
    dataflow_runtime desktop_resampling graph_document_adapter)
add_executable(rhythm_replay EXCLUDE_FROM_ALL src/main.cpp)
target_link_libraries(rhythm_replay PRIVATE rhythm_replay_support)
deploy_desktop_runtime(rhythm_replay)
