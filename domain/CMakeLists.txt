add_library(graph_domain STATIC src/graph_definition.cpp src/graph_validation.cpp
    src/processing_contracts.cpp)
target_include_directories(graph_domain PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(graph_domain PUBLIC cxx_std_20)
target_link_libraries(graph_domain PUBLIC signal_contracts)
