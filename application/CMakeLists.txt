add_library(graph_application STATIC src/graph_lifecycle.cpp src/processing_control.cpp
    src/frequency_profile_application.cpp)
target_include_directories(graph_application PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(graph_application PUBLIC cxx_std_20)
target_link_libraries(graph_application PUBLIC graph_domain)
