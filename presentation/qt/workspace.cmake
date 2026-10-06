cmake_minimum_required(VERSION 3.16)

set(CMAKE_AUTOMOC ON)

include(${CMAKE_SOURCE_DIR}/presentation/qt/sources.cmake)
set(HEADERS ${QT_PRESENTATION_HEADERS})
set(SOURCES ${QT_PRESENTATION_SOURCES})
add_library(core_app_components STATIC ${SOURCES} ${HEADERS})
add_subdirectory(${CMAKE_SOURCE_DIR}/apps/desktop_gui ${CMAKE_BINARY_DIR}/apps/desktop_gui)
target_include_directories(core_app_components PUBLIC ${CMAKE_SOURCE_DIR}/presentation/qt/include)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Gui)

set(BUILD_TESTING OFF)
set(BUILD_EXAMPLES OFF)
FetchContent_MakeAvailable(QtNodes)

target_link_libraries(core_app_components PUBLIC
        qt_engine_ports
        Qt6::Core
        aggregator-library
        config_document
        Qt6::Widgets
        QtNodes
        audio_pipeline
        analyzer
        visualization
        wled
)
