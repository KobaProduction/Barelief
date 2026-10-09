add_subdirectory(core)
add_subdirectory(aggregator)
add_subdirectory(${CMAKE_SOURCE_DIR}/adapters/desktop/configuration ${CMAKE_CURRENT_BINARY_DIR}/configuration)
add_subdirectory(${CMAKE_SOURCE_DIR}/adapters/desktop/runtime ${CMAKE_CURRENT_BINARY_DIR}/runtime)
