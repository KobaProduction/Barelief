# Qt-facing input contracts only; no concrete runtime or GUI implementation.
add_library(qt_engine_ports INTERFACE)
target_include_directories(qt_engine_ports INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(qt_engine_ports INTERFACE Qt6::Core Qt6::Network signal_contracts graph_domain)
