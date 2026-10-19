find_package(Threads REQUIRED)
# Bounded decompression of profile arrays uses the zlib shipped with Qt Core.
find_package(Qt6 REQUIRED COMPONENTS ZlibPrivate)
add_library(desktop_analysis STATIC
    ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis/offline/profile_json.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis/offline/audio_decoder.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis/offline/resampler_comparison.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis/offline/music_analysis.cpp)
set_target_properties(desktop_analysis PROPERTIES AUTOMOC OFF)
target_compile_features(desktop_analysis PUBLIC cxx_std_20)
target_include_directories(desktop_analysis PUBLIC ${CMAKE_SOURCE_DIR}/adapters/desktop/analysis)
target_include_directories(desktop_analysis SYSTEM PRIVATE ${miniaudio_SOURCE_DIR})
target_link_libraries(desktop_analysis PUBLIC Qt6::Core dsp_kernels Threads::Threads PRIVATE Qt6::ZlibPrivate desktop_resampling)
add_subdirectory(core)
add_subdirectory(aggregator)
add_subdirectory(${CMAKE_SOURCE_DIR}/adapters/desktop/configuration ${CMAKE_CURRENT_BINARY_DIR}/configuration)
add_subdirectory(${CMAKE_SOURCE_DIR}/adapters/desktop/runtime ${CMAKE_CURRENT_BINARY_DIR}/runtime)
