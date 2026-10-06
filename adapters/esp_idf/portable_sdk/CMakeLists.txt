include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")
set(sdk_dependencies bdsp)
if(PORTABLE_SDK_ESP_DSP)
    list(APPEND sdk_dependencies esp-dsp)
endif()
idf_component_register(SRCS ${PORTABLE_SDK_SOURCES}
    INCLUDE_DIRS ${PORTABLE_SDK_INCLUDES} REQUIRES ${sdk_dependencies})
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(${COMPONENT_LIB} PUBLIC PORTABLE_DSP_MAX_FFT_SIZE=512)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
if(NOT PORTABLE_SDK_ESP_DSP)
    # The vendored portable dsp_common header expects the IDF version macros
    # when __XTENSA__ is defined; provide the platform header at this boundary.
    target_compile_options(${COMPONENT_LIB} PRIVATE -include esp_idf_version.h)
endif()
