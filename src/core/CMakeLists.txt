cmake_minimum_required(VERSION 3.30)

set(LIB_NAME "core")
project(${LIB_NAME})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_library(runtime_performance INTERFACE)
target_include_directories(runtime_performance INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(${LIB_NAME} STATIC)


target_sources(${LIB_NAME} PRIVATE
        include/core/abstract/abs_task.h
        include/core/network/tcp_packet_socket.h
        include/core/network/tcp_packet_server.h
        include/core/utils.h
        include/core/latest_delivery.h
        src/abstract/abs_task.cpp
        src/network/tcp_packet_socket.cpp
        src/network/tcp_packet_server.cpp
        src/utils.cpp
)

target_include_directories(${LIB_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(${LIB_NAME} PUBLIC
        Qt6::Core
        Qt6::Network
        BDSP
)
