idf_component_register(SRCS src/nvs_persistence.cpp
    INCLUDE_DIRS include REQUIRES portable_sdk nvs_flash)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-exceptions;-fno-rtti>)
