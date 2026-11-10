add_library(domain_model STATIC src/graph_definition.cpp
    src/graph/window_parameters.cpp
    src/graph/processing_parameters.cpp src/graph/parameter_registry.cpp src/numeric/contract_snapshot.cpp
    src/numeric/parameter_descriptors.cpp src/signal/parameter_descriptors.cpp
    src/music/parameter_descriptors.cpp src/graph/routing_parameter_descriptors.cpp
    src/graph/adapter_parameter_descriptors.cpp src/graph/view_parameter_descriptors.cpp)
add_library(domain_numeric STATIC src/numeric/array_generator_contract.cpp
    src/numeric/array_concat_contract.cpp src/numeric/array_product_contract.cpp
    src/numeric/array_extraction.cpp
    src/numeric/matrix_contracts.cpp
    src/numeric/interval_operation_contracts.cpp src/numeric/common_operation_contracts.cpp
    src/numeric/sequence_operation_contracts.cpp src/numeric/spectral_sequence_parameters.cpp src/numeric/reference_operation_contracts.cpp)
add_library(domain_signal STATIC src/signal/fourier_contracts.cpp
    src/signal/window_coefficients.cpp
    src/signal/parameter_contracts.cpp src/signal/mel_contract.cpp src/signal/power_to_db_contract.cpp)
add_library(domain_music STATIC src/music/spectral_features_contract.cpp
    src/music/rhythm_contract.cpp
    src/music/tempo_evidence_contract.cpp
    src/music/onset_strength_contract.cpp
    src/music/spectral_descriptor_contract.cpp
    src/music/frequency_normalizer_contract.cpp
    src/music/musical_spectrogram_contract.cpp)
add_library(graph_domain STATIC src/graph_validation.cpp src/graph/processing_contracts.cpp
    src/numeric_parameter_bindings.cpp src/graph/preparation_vector_values.cpp src/graph/node_factory.cpp src/graph/node_validation.cpp
    src/graph/graph_admission.cpp src/graph/clock_provenance.cpp src/graph/owned_footprint.cpp
    src/graph/configuration_bindings.cpp)
foreach(module IN ITEMS domain_model domain_numeric domain_signal domain_music graph_domain)
    target_include_directories(${module} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(${module} PUBLIC cxx_std_20)
endforeach()
# The parameter registry calls numeric catalogs; declare the existing static
# link component centrally so every consumer gets CMake archive rescanning.
target_link_libraries(domain_model PUBLIC signal_contracts PRIVATE domain_numeric)
target_link_libraries(domain_numeric PUBLIC domain_model)
target_link_libraries(domain_signal PUBLIC domain_model PRIVATE window_math)
target_link_libraries(domain_music PUBLIC domain_model domain_signal)
target_link_libraries(graph_domain PUBLIC domain_model domain_numeric domain_signal domain_music)
