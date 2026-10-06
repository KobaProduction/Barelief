cmake_minimum_required(VERSION 3.30)

set(LIB_NAME "aggregator-library")
project(${LIB_NAME})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_library(${LIB_NAME} STATIC)

target_sources(${LIB_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/aggregator/abstract/types.h
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/aggregator/aggregator_server.h
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/aggregator/aggregator_client.h
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/aggregator/utils.h
)

target_sources(${LIB_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/aggregator/src/aggregator_server.cpp
        ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/aggregator/src/aggregator_client.cpp
        ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/aggregator/src/utils.cpp
)

target_include_directories(${LIB_NAME} PUBLIC
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include
)

target_link_libraries(${LIB_NAME} PUBLIC
        Qt6::Core
        Qt6::Network
        core
        BDSP
)

add_subdirectory(${CMAKE_SOURCE_DIR}/apps/registry ${CMAKE_BINARY_DIR}/apps/registry)
