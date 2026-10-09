cmake_minimum_required(VERSION 3.30)

set(LIB_NAME wled)
project(${LIB_NAME})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_library(${LIB_NAME} STATIC)

target_sources(${LIB_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/wled/abstract/types.h
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include/wled/wled_audio_reactive_server.h
)

target_sources(${LIB_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/wled/src/wled_audio_reactive_server.cpp

)

target_include_directories(${LIB_NAME} PUBLIC
        ${CMAKE_SOURCE_DIR}/adapters/desktop/include
)

target_link_libraries(${LIB_NAME} PUBLIC
        Qt6::Core
        Qt6::Network
        core
        bdsp_protocol
        wled_protocol
)


