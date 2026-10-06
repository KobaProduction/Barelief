cmake_minimum_required(VERSION 3.16)

set(CMAKE_AUTOMOC ON)

set(HEADERS
        include/core_app/graph_panel.h
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
        src/graph_panel.cpp
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
        aggregator-library
        config_document
        Qt6::Widgets
        QtNodes
        audio_pipeline
        analyzer
        visualization
        wled
)
if(WIN32 AND DEPLOY_BUILD_RUNTIME)
    find_program(QT_WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${Qt6_DIR}/../../../bin" REQUIRED NO_DEFAULT_PATH)
    get_filename_component(qt_runtime_bin "${QT_WINDEPLOYQT_EXECUTABLE}" DIRECTORY)
    get_filename_component(runtime_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(app IN ITEMS core_app)
        if(NOT TARGET ${app})
            continue()
        endif()
        add_custom_command(TARGET ${app} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E env
                "PATH=${runtime_compiler_bin};${qt_runtime_bin};$ENV{PATH}"
                "${QT_WINDEPLOYQT_EXECUTABLE}" --no-translations --compiler-runtime
                --dir "$<TARGET_FILE_DIR:${app}>" "$<TARGET_FILE:${app}>"
            COMMENT "Deploying Qt runtime for ${app}" VERBATIM)
        if(MINGW)
            add_custom_command(TARGET ${app} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_compiler_bin}/libgcc_s_seh-1.dll"
                    "${runtime_compiler_bin}/libstdc++-6.dll"
                    "${runtime_compiler_bin}/libwinpthread-1.dll"
                    "$<TARGET_FILE_DIR:${app}>" VERBATIM)
        endif()
    endforeach()
endif()
