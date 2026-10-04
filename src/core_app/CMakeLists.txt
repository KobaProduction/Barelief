cmake_minimum_required(VERSION 3.16)

set(CMAKE_AUTOMOC ON)

set(HEADERS
        include/core_app/types.h
        include/core_app/gui_presenter.h
        include/core_app/core_application.h
        include/core_app/config_manager.h
        include/core_app/settings_widget.h
        include/core_app/main_window.h
        include/core_app/scheme_editor/audio_node_model.h
        include/core_app/scheme_editor/editor_widget.h
        include/core_app/scheme_editor/sink_data_model.h
        include/core_app/scheme_editor/source_data_model.h
        include/core_app/scheme_editor/processor_data_model.h
        include/core_app/scheme_editor/types.h
)

set(SOURCES
        src/gui_presenter.cpp
        src/core_application.cpp
        src/config_manager.cpp
        src/main_window.cpp
        src/settings_widget.cpp
        src/camera_controls_settings.cpp

        src/scheme_editor/audio_node_model.cpp
        src/scheme_editor/editor_widget.cpp

        src/scheme_editor/sink_data_model.cpp
        src/scheme_editor/source_data_model.cpp
        src/scheme_editor/processor_data_model.cpp
        src/scheme_editor/types.cpp
)

add_library(core_app_components STATIC ${SOURCES} ${HEADERS})
add_executable(core_app src/main.cpp)
target_link_libraries(core_app PRIVATE core_app_components)
target_include_directories(core_app_components PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Gui)

set(BUILD_TESTING OFF)
set(BUILD_EXAMPLES OFF)
FetchContent_MakeAvailable(QtNodes)

target_link_libraries(core_app_components PUBLIC
        Qt6::Core
        Qt6::Widgets
        QtNodes
        audio_pipeline
        analyzer
        visualization
        wled
)
