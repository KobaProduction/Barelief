# Match the compiler target, not the machine running CMake. Keep windeployqt's
# own diagnostics when the target or the installed DXC pair cannot be identified.
function(find_windows_sdk_dxc_redist output)
    set(${output} "" PARENT_SCOPE)
    string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" architecture)
    if(NOT architecture AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        get_property(target_queried GLOBAL PROPERTY BARELIEF_DXC_COMPILER_TARGET_QUERIED)
        if(NOT target_queried)
            execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
                RESULT_VARIABLE target_status OUTPUT_VARIABLE compiler_target
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET TIMEOUT 5)
            set_property(GLOBAL PROPERTY BARELIEF_DXC_COMPILER_TARGET_QUERIED TRUE)
            if(target_status STREQUAL "0")
                set_property(GLOBAL PROPERTY BARELIEF_DXC_COMPILER_TARGET "${compiler_target}")
            endif()
        endif()
        get_property(compiler_target GLOBAL PROPERTY BARELIEF_DXC_COMPILER_TARGET)
        if(compiler_target MATCHES "^(x86_64|aarch64|i[3-6]86)-[a-zA-Z0-9_-]+$")
            string(REGEX REPLACE "-.*$" "" architecture "${compiler_target}")
        endif()
    endif()
    if(architecture MATCHES "^(x86_64|amd64|x64)$" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(architecture x64)
    elseif(architecture MATCHES "^(aarch64|arm64)$" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(architecture arm64)
    elseif(architecture MATCHES "^(x86|i[3-6]86)$" AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(architecture x86)
    else()
        return()
    endif()
    set(sdk_roots "$ENV{WindowsSdkDir}")
    if(CMAKE_HOST_WIN32)
        cmake_host_system_information(RESULT installed_sdk
            QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
            VALUE KitsRoot10 VIEW BOTH ERROR_VARIABLE sdk_registry_error)
        if(NOT sdk_registry_error)
            list(APPEND sdk_roots "${installed_sdk}")
        endif()
    endif()
    list(REMOVE_DUPLICATES sdk_roots)
    foreach(sdk_root IN LISTS sdk_roots)
        if(NOT sdk_root)
            continue()
        endif()
        file(TO_CMAKE_PATH "${sdk_root}/Redist/D3D/${architecture}" redist)
        if(EXISTS "${redist}/dxcompiler.dll" AND EXISTS "${redist}/dxil.dll")
            set(${output} "${redist}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

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
    find_windows_sdk_dxc_redist(dxc_redist_bin)
    set(runtime_deploy_path "${runtime_compiler_bin};${qt_runtime_bin};$ENV{PATH}")
    if(dxc_redist_bin)
        string(PREPEND runtime_deploy_path "${dxc_redist_bin};")
    endif()
    set(qml_deploy_args)
    if(ARGV1 STREQUAL "QML")
        list(APPEND qml_deploy_args --qmldir "${CMAKE_SOURCE_DIR}/presentation/qt/qml")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=${runtime_deploy_path}"
            "${QT_WINDEPLOYQT_EXECUTABLE}" --no-translations --compiler-runtime ${qml_deploy_args}
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
