idf_component_register(SRCS src/byte_transport.cpp INCLUDE_DIRS include
    REQUIRES portable_sdk esp_driver_uart lwip freertos)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-exceptions;-fno-rtti>)
