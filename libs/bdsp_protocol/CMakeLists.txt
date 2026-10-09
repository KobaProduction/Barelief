add_subdirectory(vendor/tinycbor)
add_subdirectory(vendor/nanopb)
add_library(bdsp_protocol STATIC src/byte_stream.cpp src/stream_fragments.cpp
    src/numeric_data_message.cpp src/numeric_data_codec.cpp
    src/numeric_control_message.cpp src/numeric_control_codec.cpp
    generated/numeric_control.pb.c)
target_include_directories(bdsp_protocol PRIVATE generated)
target_compile_features(bdsp_protocol PUBLIC cxx_std_20)
target_include_directories(bdsp_protocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(bdsp_protocol PUBLIC signal_contracts graph_domain BDSP::BDSP wled_protocol)
target_link_libraries(bdsp_protocol PRIVATE tinycbor_runtime nanopb_runtime)
install(TARGETS bdsp_protocol EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
