cmake_minimum_required(VERSION 3.30)

set(LIB_NAME "core")
project(${LIB_NAME})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network SerialPort)

add_library(runtime_performance INTERFACE)
target_include_directories(runtime_performance INTERFACE ${CMAKE_SOURCE_DIR}/adapters/desktop/include)
include(${CMAKE_SOURCE_DIR}/cmake/dev_telemetry.cmake)

add_library(${LIB_NAME} STATIC)
target_link_libraries(${LIB_NAME} PUBLIC qt_engine_ports)
target_link_libraries(${LIB_NAME} PUBLIC runtime_performance)


target_sources(${LIB_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/core/abstract/abs_task.h
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/core/network/tcp_packet_socket.h
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/core/network/tcp_packet_server.h
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/core/utils.h
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/core/latest_delivery.h
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/desktop/qt_byte_transport.h
        ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/core/src/qt_byte_transport.cpp
        ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/core/src/processing_timer_policy.cpp
        ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/core/src/display_refresh_rates.cpp
        ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/core/src/abstract/abs_task.cpp
        ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/core/src/network/tcp_packet_socket.cpp
        ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/core/src/network/tcp_packet_server.cpp
        ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/core/src/utils.cpp
)

if(WIN32)
    target_link_libraries(${LIB_NAME} PRIVATE user32)
endif()

target_include_directories(${LIB_NAME} PUBLIC
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include
)

target_link_libraries(${LIB_NAME} PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::SerialPort
        bdsp_protocol
        BDSP
)
