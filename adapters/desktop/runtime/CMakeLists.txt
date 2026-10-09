set(desktop_pipeline_sources ${CMAKE_SOURCE_DIR}/adapters/desktop/platform/pipeline/src)
set(desktop_runtime_sources ${CMAKE_SOURCE_DIR}/adapters/desktop/runtime/src)
add_library(audio_pipeline STATIC ${desktop_runtime_sources}/numeric_engine.cpp ${desktop_pipeline_sources}/audio_source.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/include/pipeline/audio_source.h
    ${CMAKE_SOURCE_DIR}/adapters/desktop/include/pipeline/numeric_engine.h)
add_library(desktop_numeric_defaults STATIC ${CMAKE_SOURCE_DIR}/apps/desktop_gui/src/numeric_default_graph.cpp)
set_target_properties(desktop_numeric_defaults PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_features(desktop_numeric_defaults PUBLIC cxx_std_20)
target_include_directories(desktop_numeric_defaults PUBLIC ${CMAKE_SOURCE_DIR}/apps/desktop_gui/include)
target_link_libraries(desktop_numeric_defaults PUBLIC graph_domain)
add_library(miniaudio_capture STATIC ${desktop_pipeline_sources}/miniaudio_impl.c)
target_sources(audio_pipeline PRIVATE ${desktop_pipeline_sources}/test_signal.cpp)
set_target_properties(miniaudio_capture PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(miniaudio_capture PUBLIC ${miniaudio_SOURCE_DIR})
target_compile_definitions(miniaudio_capture PUBLIC MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION)
find_package(Threads REQUIRED)
target_link_libraries(miniaudio_capture PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
    target_link_libraries(audio_pipeline PRIVATE PkgConfig::PULSE)
endif()
target_link_libraries(audio_pipeline PRIVATE miniaudio_capture)
if(WIN32)
    target_sources(audio_pipeline PRIVATE ${desktop_pipeline_sources}/default_device_watcher_windows.cpp)
    target_link_libraries(audio_pipeline PRIVATE ole32 uuid)
endif()
target_include_directories(audio_pipeline PUBLIC ${CMAKE_SOURCE_DIR}/adapters/desktop/include)
target_link_libraries(audio_pipeline PUBLIC qt_engine_ports core Qt6::Core Qt6::Network graph_document_adapter dataflow_runtime portable_control_mapper)
add_subdirectory(${CMAKE_SOURCE_DIR}/apps/desktop_core ${CMAKE_BINARY_DIR}/apps/desktop_core)
target_link_libraries(audio_pipeline PUBLIC aggregator-library)
add_library(desktop_numeric_remote STATIC ${desktop_runtime_sources}/numeric_remote_endpoint.cpp
    ${desktop_runtime_sources}/numeric_control_server.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/include/control/numeric_remote_endpoint.h
    ${CMAKE_SOURCE_DIR}/adapters/desktop/include/control/numeric_control_server.h)
target_include_directories(desktop_numeric_remote PUBLIC ${CMAKE_SOURCE_DIR}/adapters/desktop/include)
target_link_libraries(desktop_numeric_remote PUBLIC audio_pipeline portable_numeric_control_session core
    graph_document_adapter Qt6::Core Qt6::Network Qt6::SerialPort)

add_library(desktop_numeric_routing STATIC ${desktop_runtime_sources}/numeric_routing_adapter.cpp
    ${desktop_runtime_sources}/numeric_desktop_host.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/include/control/numeric_desktop_host.h
    ${CMAKE_SOURCE_DIR}/adapters/desktop/include/control/numeric_routing_adapter.h)
target_include_directories(desktop_numeric_routing PUBLIC ${CMAKE_SOURCE_DIR}/adapters/desktop/include)
target_link_libraries(desktop_numeric_routing PUBLIC desktop_numeric_remote aggregator-library)
