cmake_minimum_required(VERSION 3.30)

set(LIB_NAME "aggregator-library")
project(${LIB_NAME})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_library(${LIB_NAME} STATIC)

target_sources(${LIB_NAME} PRIVATE
        include/aggregator/abstract/types.h
        include/aggregator/aggregator_server.h
        include/aggregator/aggregator_client.h
        include/aggregator/utils.h
)

target_sources(${LIB_NAME} PRIVATE
        src/aggregator_server.cpp
        src/aggregator_client.cpp
        src/utils.cpp
)

target_include_directories(${LIB_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(${LIB_NAME} PUBLIC
        Qt6::Core
        Qt6::Network
        core
        BDSP
)

add_executable(aggregator_server_app src/main_server.cpp)
target_link_libraries(aggregator_server_app PRIVATE ${LIB_NAME})

add_executable(aggregator_client_app src/main_client.cpp)
target_link_libraries(aggregator_client_app PRIVATE ${LIB_NAME})


