add_library(dataflow_runtime STATIC src/runtime.cpp)
target_include_directories(dataflow_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(dataflow_runtime PUBLIC cxx_std_20)
target_link_libraries(dataflow_runtime PUBLIC graph_application dsp_kernels wled_protocol)
install(TARGETS dataflow_runtime EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
