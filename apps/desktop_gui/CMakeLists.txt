add_executable(core_app main.cpp)
target_link_libraries(core_app PRIVATE core_app_components)
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
