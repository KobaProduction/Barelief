# Optional developer tool, separate from application builds and installation.
if(WIN32)
    find_program(BARELIEF_POWERSHELL NAMES pwsh powershell)
    if(BARELIEF_POWERSHELL)
        add_custom_target(serena-mcp
            COMMAND "${BARELIEF_POWERSHELL}" -NoProfile -File
                "${PROJECT_SOURCE_DIR}/scripts/start_serena.ps1"
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "Start or reuse the shared Barelief Serena MCP server"
            VERBATIM)
    endif()
endif()
