add_library(desktop_gui_configuration STATIC
    ${CMAKE_SOURCE_DIR}/adapters/desktop/gui_config/src/config_manager.cpp
    ${CMAKE_SOURCE_DIR}/adapters/desktop/gui_config/include/core_app/config_manager.h)
target_include_directories(desktop_gui_configuration PUBLIC
    ${CMAKE_SOURCE_DIR}/adapters/desktop/gui_config/include
    ${CMAKE_SOURCE_DIR}/presentation/qt/include)
target_link_libraries(desktop_gui_configuration PUBLIC config_document visualization audio_pipeline aggregator-library Qt6::Widgets QtNodes)
add_library(desktop_gui_composition STATIC src/core_application.cpp include/core_app/core_application.h)
target_include_directories(desktop_gui_composition PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(desktop_gui_composition PUBLIC core_app_components desktop_gui_configuration)
add_executable(core_app main.cpp)
target_link_libraries(core_app PRIVATE desktop_gui_composition)
deploy_desktop_runtime(core_app)
