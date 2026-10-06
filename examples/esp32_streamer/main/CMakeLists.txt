idf_component_register(SRCS main.cpp INCLUDE_DIRS . REQUIRES stream_adapter esp_timer)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti)
