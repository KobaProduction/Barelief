cmake_minimum_required(VERSION 3.30)
project(portable_sdk_consumer LANGUAGES CXX)
find_package(PortableDSP REQUIRED CONFIG)
add_executable(sdk_consumer main.cpp)
target_compile_features(sdk_consumer PRIVATE cxx_std_20)
target_link_libraries(sdk_consumer PRIVATE PortableDSP::dsp_kernels PortableDSP::signal_contracts
    PortableDSP::bdsp_protocol PortableDSP::graph_application PortableDSP::dataflow_runtime
    PortableDSP::embedded_support PortableDSP::portable_numeric_control_session)
if(NOT MSVC)
    target_compile_options(sdk_consumer PRIVATE -fno-exceptions -fno-rtti)
endif()
