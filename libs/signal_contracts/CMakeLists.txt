add_library(signal_contracts STATIC src/frame.cpp src/buffer_pool.cpp
    src/data_contract.cpp src/data_frame.cpp src/numeric_frame_gather.cpp)
target_compile_features(signal_contracts PUBLIC cxx_std_20)
target_include_directories(signal_contracts PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
install(TARGETS signal_contracts EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
