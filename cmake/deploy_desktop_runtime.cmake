# Deploy the dependencies of each executable, including headless-only builds.
# Composition roots decide which applications exist; deployment follows targets.
function(deploy_desktop_runtime target)
    if(NOT WIN32 OR NOT DEPLOY_BUILD_RUNTIME OR NOT TARGET ${target})
        return()
    endif()
    find_program(QT_WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${Qt6_DIR}/../../../bin" REQUIRED NO_DEFAULT_PATH)
    get_filename_component(qt_runtime_bin "${QT_WINDEPLOYQT_EXECUTABLE}" DIRECTORY)
    get_filename_component(runtime_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=${runtime_compiler_bin};${qt_runtime_bin};$ENV{PATH}"
            "${QT_WINDEPLOYQT_EXECUTABLE}" --no-translations --compiler-runtime
            --dir "$<TARGET_FILE_DIR:${target}>" "$<TARGET_FILE:${target}>"
        COMMENT "Deploying Qt runtime for ${target}" VERBATIM)
    if(MINGW)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${runtime_compiler_bin}/libgcc_s_seh-1.dll"
                "${runtime_compiler_bin}/libstdc++-6.dll"
                "${runtime_compiler_bin}/libwinpthread-1.dll"
                "$<TARGET_FILE_DIR:${target}>" VERBATIM)
    endif()
endfunction()
