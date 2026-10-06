add_library(config_document INTERFACE)
target_include_directories(config_document INTERFACE ${CMAKE_SOURCE_DIR}/adapters/desktop/include)
target_link_libraries(config_document INTERFACE Qt6::Core)
