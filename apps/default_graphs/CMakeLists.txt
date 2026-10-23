# Example/startup composition belongs to applications, never to a device adapter.
add_library(desktop_numeric_defaults STATIC
    src/example_builder.cpp
    src/numeric_default_graph.cpp
    src/numeric_rhythm_graph.cpp
    src/numeric_music_graph.cpp)
set_target_properties(desktop_numeric_defaults PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_features(desktop_numeric_defaults PUBLIC cxx_std_20)
target_include_directories(desktop_numeric_defaults PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(desktop_numeric_defaults PUBLIC graph_application PRIVATE window_math)
