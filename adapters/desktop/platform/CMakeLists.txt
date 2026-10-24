find_package(Threads REQUIRED)
# Use the system zlib ABI on Linux; Qt's bundled private Zlib target is
# Windows-specific and is not an exported component of the Linux Qt SDK.
if(WIN32)
    find_package(Qt6 REQUIRED COMPONENTS ZlibPrivate)
    set(BARELIEF_ZLIB_TARGET Qt6::ZlibPrivate)
else()
    find_package(ZLIB REQUIRED)
    set(BARELIEF_ZLIB_TARGET ZLIB::ZLIB)
endif()
add_library(desktop_analysis STATIC
    ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis/offline/profile_json.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis/offline/curve_json.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis/offline/scan_json.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis/offline/audio_decoder.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis/offline/resampler_comparison.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis/offline/music_analysis.cpp)
set_target_properties(desktop_analysis PROPERTIES AUTOMOC OFF)
target_compile_features(desktop_analysis PUBLIC cxx_std_20)
target_include_directories(desktop_analysis PUBLIC ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis)
target_include_directories(desktop_analysis SYSTEM PRIVATE ${miniaudio_SOURCE_DIR})
target_link_libraries(desktop_analysis PUBLIC Qt6::Core dsp_kernels Threads::Threads PRIVATE ${BARELIEF_ZLIB_TARGET} desktop_resampling)
add_subdirectory(core)
add_subdirectory(aggregator)
add_subdirectory(${CMAKE_SOURCE_DIR}/adapters/desktop/configuration ${CMAKE_CURRENT_BINARY_DIR}/configuration)
add_subdirectory(${CMAKE_SOURCE_DIR}/adapters/desktop/runtime ${CMAKE_CURRENT_BINARY_DIR}/runtime)
