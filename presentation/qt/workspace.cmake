cmake_minimum_required(VERSION 3.16)

set(CMAKE_AUTOMOC ON)
include(${CMAKE_SOURCE_DIR}/cmake/qt_workspace.cmake)

include(${CMAKE_SOURCE_DIR}/presentation/qt/sources.cmake)
set(HEADERS ${QT_PRESENTATION_HEADERS})
set(SOURCES ${QT_PRESENTATION_SOURCES})
add_library(core_app_components STATIC ${SOURCES} ${HEADERS})
target_sources(core_app_components PRIVATE src/resampler_comparison_dialog.cpp)
target_sources(core_app_components PRIVATE src/music_analysis_dialog.cpp)
target_sources(core_app_components PRIVATE src/frequency_profile_binding.cpp)
target_link_libraries(core_app_components PUBLIC desktop_resampling)
target_link_libraries(core_app_components PUBLIC desktop_analysis)
target_link_libraries(core_app_components PRIVATE numeric_array_codec dsp_kernels)
target_sources(core_app_components PRIVATE src/renderer/numeric_view_adapter.cpp
    include/core_app/numeric_view_adapter.h)
target_include_directories(core_app_components PUBLIC ${CMAKE_SOURCE_DIR}/presentation/qt/include)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Gui)

set(BUILD_TESTING OFF)
set(BUILD_EXAMPLES OFF)
FetchContent_MakeAvailable(QtNodes)
include("${CMAKE_SOURCE_DIR}/cmake/patch_qtnodes_scene_lifetime.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/patch_qtnodes_connection_undo.cmake")

target_link_libraries(core_app_components PUBLIC
        qt_workspace
        qt_engine_ports
        Qt6::Core
        aggregator-library
        config_document
        graph_document_adapter
        Qt6::Widgets
        QtNodes
        audio_pipeline
        wled_protocol
        visualization
)
