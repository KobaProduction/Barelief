add_library(desktop_gui_configuration STATIC
    ${CMAKE_SOURCE_DIR}/adapters/desktop/gui_config/src/config_manager.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/gui_config/include/core_app/config_manager.h)
target_include_directories(desktop_gui_configuration PUBLIC
    ${CMAKE_SOURCE_DIR}/adapters/desktop/gui_config/include
    ${CMAKE_SOURCE_DIR}/presentation/qt/include)
target_link_libraries(desktop_gui_configuration PUBLIC config_document graph_document_adapter barelief audio_pipeline aggregator-library Qt6::Widgets QtNodes)
add_library(desktop_gui_composition STATIC src/core_application.cpp src/analysis_services.cpp
    src/core_application_startup.cpp src/core_application_actions.cpp
    src/core_application_display.cpp src/core_application_persistence.cpp src/core_application_autosave.cpp
    src/core_application_save_events.cpp
    src/core_application_arguments.cpp src/core_application_state.h
    src/example_presentation.cpp src/example_actions.cpp include/core_app/core_application.h)
target_sources(desktop_gui_composition PRIVATE src/product_identity.cpp src/product_information.cpp src/product_updates.cpp)
find_package(Qt6 REQUIRED COMPONENTS Svg)
target_link_libraries(desktop_gui_composition PRIVATE Qt6::Svg)
configure_file(include/core_app/product_identity.h.in generated/core_app/product_identity.h @ONLY)
target_include_directories(desktop_gui_composition PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
qt_add_resources(desktop_gui_composition barelief_branding PREFIX "/barelief"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES resources/barelief.svg)
file(GLOB_RECURSE barelief_license_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/packaging/licenses/*")
qt_add_resources(desktop_gui_composition barelief_licenses PREFIX "/barelief"
    BASE "${CMAKE_SOURCE_DIR}" FILES "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" ${barelief_license_files})
target_include_directories(desktop_gui_composition PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(desktop_gui_composition PUBLIC core_app_components desktop_gui_configuration desktop_numeric_defaults desktop_numeric_remote desktop_numeric_routing desktop_processor_discovery
    PRIVATE desktop_analysis desktop_resampling)
add_executable(core_app main.cpp package_verification_probe.cpp)
if(BARELIEF_DEV_TELEMETRY)
    target_sources(core_app PRIVATE dev_telemetry_ui.cpp)
    set(BARELIEF_TRACY_PROFILER_PATH "${CMAKE_SOURCE_DIR}/.agent-workspace/tools/tracy/0.14.1/tracy-profiler.exe"
        CACHE FILEPATH "Developer Tracy viewer")
    target_compile_definitions(core_app PRIVATE BARELIEF_TRACY_PROFILER_PATH="${BARELIEF_TRACY_PROFILER_PATH}")
endif()
set_target_properties(core_app PROPERTIES OUTPUT_NAME Barelief)
target_link_libraries(core_app PRIVATE desktop_gui_composition)
if(WIN32)
    enable_language(RC)
    string(REPLACE "." "," BARELIEF_WINDOWS_VERSION "${PROJECT_VERSION}.0")
    configure_file(resources/barelief.rc.in barelief.rc @ONLY)
    target_sources(core_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/barelief.rc")
    target_link_libraries(core_app PRIVATE ole32)
endif()
deploy_desktop_runtime(core_app QML)
