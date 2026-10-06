# Transitional target for existing desktop consumers.
if(NOT TARGET dsp_kernels)
 add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../libs/dsp_kernels ${CMAKE_CURRENT_BINARY_DIR}/dsp_kernels)
endif()
add_library(portable_dsp INTERFACE)
target_include_directories(portable_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(portable_dsp INTERFACE dsp_kernels)
