# Explicit source dependency, pinned by the consuming firmware. No fetch/install.
if(NOT BDSP_SOURCE_DIR OR NOT EXISTS "${BDSP_SOURCE_DIR}/include/BDSP/types.h")
    message(FATAL_ERROR "BDSP_SOURCE_DIR must point to BDSP commit 2c68056dfaeffba2f34fc64eee9bc9e331e83c7c")
endif()
set(bdsp_sources
    mixins.cpp receiver.cpp transmitter.cpp memory_manager.cpp checksums/crc8.cpp
    streams/mixins.cpp streams/reader.cpp streams/writer.cpp
    strategies/abstract/read.cpp strategies/abstract/write.cpp
    strategies/byte_stuffing/mixins.cpp strategies/byte_stuffing/read.cpp strategies/byte_stuffing/write.cpp
    strategies/cobs/mixins.cpp strategies/cobs/read.cpp strategies/cobs/write.cpp)
list(TRANSFORM bdsp_sources PREPEND "${BDSP_SOURCE_DIR}/src/")
idf_component_register(SRCS ${bdsp_sources} INCLUDE_DIRS "${BDSP_SOURCE_DIR}/include")
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(${COMPONENT_LIB} PUBLIC BDSP_STATIC_DEFINE)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
