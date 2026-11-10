# Parallel targets deploy into the same output directory. Keep the Qt scan/copy
# and compiler-runtime copy in one critical section; always propagate failures.
file(LOCK "${DEPLOY_DIRECTORY}/.barelief-runtime-deploy.lock" GUARD PROCESS TIMEOUT 300)
set(qml_arguments)
if(DEPLOY_QML_DIRECTORY)
    list(APPEND qml_arguments --qmldir "${DEPLOY_QML_DIRECTORY}")
endif()
execute_process(
    COMMAND "${DEPLOY_QT_TOOL}" --no-translations --compiler-runtime ${qml_arguments}
        --dir "${DEPLOY_DIRECTORY}" "${DEPLOY_EXECUTABLE}"
    RESULT_VARIABLE deploy_status)
if(NOT deploy_status STREQUAL "0")
    message(FATAL_ERROR "Qt runtime deployment failed: ${deploy_status}")
endif()
if(DEPLOY_COMPILER_DIRECTORY)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${DEPLOY_COMPILER_DIRECTORY}/libgcc_s_seh-1.dll"
        "${DEPLOY_COMPILER_DIRECTORY}/libstdc++-6.dll"
        "${DEPLOY_COMPILER_DIRECTORY}/libwinpthread-1.dll"
        "${DEPLOY_DIRECTORY}"
        RESULT_VARIABLE compiler_status)
    if(NOT compiler_status STREQUAL "0")
        message(FATAL_ERROR "Compiler runtime deployment failed: ${compiler_status}")
    endif()
endif()
