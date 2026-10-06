add_library(bdsp_protocol STATIC src/stream_codec.cpp src/byte_stream.cpp src/stream_fragments.cpp)
target_compile_features(bdsp_protocol PUBLIC cxx_std_20)
target_include_directories(bdsp_protocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(bdsp_protocol PUBLIC signal_contracts BDSP::BDSP)
install(TARGETS bdsp_protocol EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
