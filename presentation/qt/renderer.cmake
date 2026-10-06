add_library(visualization STATIC)
add_library(qt_touchpad_input STATIC ${CMAKE_CURRENT_LIST_DIR}/src/editor/touchpad_input.cpp)
target_include_directories(qt_touchpad_input PUBLIC ${CMAKE_SOURCE_DIR}/presentation/qt/include)
target_link_libraries(qt_touchpad_input PUBLIC Qt6::Widgets)
if(WIN32)
    target_link_libraries(qt_touchpad_input PRIVATE ole32)
endif()

set(CMAKE_AUTOMOC ON)
#set(CMAKE_AUTORCC ON)
#set(CMAKE_AUTOUIC ON)

include(${CMAKE_SOURCE_DIR}/presentation/qt/sources.cmake)
set(HEADERS ${QT_RENDERER_HEADERS})
set(SOURCES ${QT_RENDERER_SOURCES})
target_sources(visualization PRIVATE ${HEADERS} ${SOURCES})

target_include_directories(visualization PUBLIC ${CMAKE_SOURCE_DIR}/presentation/qt/include)

find_package(Qt6 REQUIRED COMPONENTS Charts DataVisualization OpenGLWidgets OpenGL)
find_package(Qt6 REQUIRED COMPONENTS LinguistTools)
qt_add_lrelease(visualization TS_FILES ${QT_PRESENTATION_RESOURCE_DIR}/i18n/ui_ru.ts
    QM_FILES_OUTPUT_VARIABLE ui_qm_files)
qt_add_resources(visualization ui_translations PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${ui_qm_files})

target_link_libraries(visualization PUBLIC Qt::Charts Qt::DataVisualization Qt::OpenGLWidgets Qt::OpenGL runtime_performance qt_touchpad_input)

qt_add_resources(visualization ui_theme_icons PREFIX "/ui-theme"
    BASE "${QT_PRESENTATION_RESOURCE_DIR}/ui_theme" FILES ${QT_PRESENTATION_RESOURCE_DIR}/ui_theme/checkmark.svg ${QT_PRESENTATION_RESOURCE_DIR}/ui_theme/minus.svg)
