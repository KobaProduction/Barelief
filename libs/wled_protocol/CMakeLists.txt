add_library(wled_protocol STATIC src/wled_packet.cpp)
target_compile_features(wled_protocol PUBLIC cxx_std_20)
target_include_directories(wled_protocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
install(TARGETS wled_protocol EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
