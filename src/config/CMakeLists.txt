add_library(config_document INTERFACE)
target_include_directories(config_document INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(config_document INTERFACE Qt6::Core)
