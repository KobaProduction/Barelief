# Explicit optional GPL/AGPL research composition, absent from ALL and SDK.
add_library(rhythm_reference_graphs STATIC EXCLUDE_FROM_ALL
    ../default_graphs/src/numeric_aubio_reference_graph.cpp
    ../default_graphs/src/numeric_reference_graphs.cpp
    ../default_graphs/src/numeric_btrack_reference_graph.cpp
    ../default_graphs/src/numeric_madmom_reference_graph.cpp
    ../default_graphs/src/numeric_essentia_reference_graph.cpp
    ../default_graphs/src/essentia_graph_parts.cpp)
set_target_properties(rhythm_reference_graphs PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(rhythm_reference_graphs PUBLIC desktop_numeric_defaults rhythm_graph_adapters)
target_compile_features(rhythm_reference_graphs PUBLIC cxx_std_20)
add_executable(rhythm_reference_runner EXCLUDE_FROM_ALL main.cpp export_graphs.cpp value_probe.cpp
    fourier_profile.cpp research_profile.cpp offline_run.cpp wave_input.cpp)
target_link_libraries(rhythm_reference_runner PRIVATE rhythm_reference_graphs audio_pipeline
    desktop_numeric_remote desktop_resampling desktop_fourier_backend desktop_analysis graph_document_adapter Qt6::Core Qt6::Network)
target_compile_features(rhythm_reference_runner PRIVATE cxx_std_20)
deploy_desktop_runtime(rhythm_reference_runner)
if(BUILD_TESTING)
    add_executable(rhythm_whole_input_tests EXCLUDE_FROM_ALL
        ${PROJECT_SOURCE_DIR}/tests/rhythm_whole_input_tests.cpp offline_run.cpp research_profile.cpp fourier_profile.cpp wave_input.cpp)
    target_include_directories(rhythm_whole_input_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(rhythm_whole_input_tests PRIVATE rhythm_reference_graphs desktop_analysis
        desktop_resampling desktop_fourier_backend graph_document_adapter Qt6::Core)
    set_target_properties(rhythm_whole_input_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    add_test(NAME rhythm_whole_input_tests COMMAND rhythm_whole_input_tests)
    set_tests_properties(rhythm_whole_input_tests PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE LABELS "research;reference;regression;whole-input")
    add_executable(research_fourier_profile_tests EXCLUDE_FROM_ALL
        ${PROJECT_SOURCE_DIR}/tests/research_fourier_profile_tests.cpp fourier_profile.cpp)
    target_include_directories(research_fourier_profile_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(research_fourier_profile_tests PRIVATE domain_numeric desktop_fourier_backend)
    set_target_properties(research_fourier_profile_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    add_test(NAME research_fourier_profile_tests COMMAND research_fourier_profile_tests)
    set_tests_properties(research_fourier_profile_tests PROPERTIES TIMEOUT 30 LABELS "numeric;reference;regression;fft")
    add_executable(real_fourier_provider_tests EXCLUDE_FROM_ALL ${PROJECT_SOURCE_DIR}/tests/real_fourier_provider_tests.cpp)
    target_link_libraries(real_fourier_provider_tests PRIVATE dataflow_runtime desktop_fourier_backend)
    set_target_properties(real_fourier_provider_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    add_test(NAME real_fourier_provider_tests COMMAND real_fourier_provider_tests)
    set_tests_properties(real_fourier_provider_tests PROPERTIES TIMEOUT 60 LABELS "numeric;reference;regression;fft")
    add_executable(research_control_profile_tests EXCLUDE_FROM_ALL ${PROJECT_SOURCE_DIR}/tests/research_control_profile_tests.cpp)
    target_link_libraries(research_control_profile_tests PRIVATE desktop_numeric_remote dataflow_runtime)
    add_test(NAME research_control_profile_tests COMMAND research_control_profile_tests)
    set_tests_properties(research_control_profile_tests PROPERTIES TIMEOUT 30 LABELS "numeric;reference;regression")
    add_executable(reference_array_geometry_tests EXCLUDE_FROM_ALL ${PROJECT_SOURCE_DIR}/tests/reference_array_geometry_tests.cpp)
    target_link_libraries(reference_array_geometry_tests PRIVATE dsp_graph_adapters)
    add_test(NAME reference_array_geometry_tests COMMAND reference_array_geometry_tests)
    set_tests_properties(reference_array_geometry_tests PROPERTIES TIMEOUT 30 LABELS "numeric;reference;regression")
    add_executable(aubio_reference_pipeline_tests EXCLUDE_FROM_ALL
        ${PROJECT_SOURCE_DIR}/tests/aubio_reference_pipeline_tests.cpp)
    target_link_libraries(aubio_reference_pipeline_tests PRIVATE rhythm_reference_graphs desktop_fourier_backend)
    add_test(NAME aubio_reference_pipeline_tests COMMAND aubio_reference_pipeline_tests)
    set_tests_properties(aubio_reference_pipeline_tests PROPERTIES TIMEOUT 180 LABELS "research;reference;aubio")
    foreach(library btrack madmom essentia)
        add_executable(${library}_reference_pipeline_tests EXCLUDE_FROM_ALL
            ${PROJECT_SOURCE_DIR}/tests/${library}_reference_pipeline_tests.cpp)
        target_link_libraries(${library}_reference_pipeline_tests PRIVATE rhythm_reference_graphs desktop_resampling)
        add_test(NAME ${library}_reference_pipeline_tests COMMAND ${library}_reference_pipeline_tests)
        set_tests_properties(${library}_reference_pipeline_tests PROPERTIES TIMEOUT 120 LABELS "research;reference;${library}")
    endforeach()
    target_link_libraries(btrack_reference_pipeline_tests PRIVATE desktop_fourier_backend)
    target_link_libraries(essentia_reference_pipeline_tests PRIVATE desktop_fourier_backend)
    # Full three-mode, packet/replay and rollover coverage measured 227 seconds.
    set_tests_properties(madmom_reference_pipeline_tests PROPERTIES TIMEOUT 600)
    # Registered CTest executables must exist after the ordinary test build,
    # even though the optional research application stays outside ALL.
    get_property(reference_test_targets DIRECTORY PROPERTY TESTS)
    foreach(reference_test_target IN LISTS reference_test_targets)
        set_property(TARGET ${reference_test_target} PROPERTY EXCLUDE_FROM_ALL FALSE)
    endforeach()
endif()
