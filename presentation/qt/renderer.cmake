add_library(barelief STATIC)
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
target_sources(barelief PRIVATE ${HEADERS} ${SOURCES})
target_sources(barelief PRIVATE src/renderer/numeric_series_view.cpp include/barelief/numeric_series_view.h)

target_include_directories(barelief PUBLIC ${CMAKE_SOURCE_DIR}/presentation/qt/include)

find_package(Qt6 REQUIRED COMPONENTS Qml Quick QuickWidgets QuickControls2 OpenGL )
find_package(Qt6 REQUIRED COMPONENTS LinguistTools)
qt_add_lrelease(barelief TS_FILES ${QT_PRESENTATION_RESOURCE_DIR}/i18n/ui_ru.ts
    QM_FILES_OUTPUT_VARIABLE ui_qm_files)
qt_add_resources(barelief ui_translations PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${ui_qm_files})

target_link_libraries(barelief PUBLIC Qt6::Qml Qt6::Quick Qt6::QuickWidgets Qt6::QuickControls2 Qt::OpenGL  runtime_performance qt_touchpad_input)

qt_add_resources(barelief ui_theme_icons PREFIX "/ui-theme"
    BASE "${QT_PRESENTATION_RESOURCE_DIR}/ui_theme" FILES ${QT_PRESENTATION_RESOURCE_DIR}/ui_theme/checkmark.svg ${QT_PRESENTATION_RESOURCE_DIR}/ui_theme/minus.svg)

qt_add_resources(barelief barelief_qml PREFIX "/"
    BASE "${QT_PRESENTATION_RESOURCE_DIR}/qml"
    FILES
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/common/GraphPanel.qml"
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/plots2d/Plot2D.qml"
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/plots2d/Piano.qml"
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/surface3d/SurfaceView.qml"
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/surface3d/SurfaceControls.qml"
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/controls/CompactButton.qml"
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/controls/CompactCheckBox.qml"
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/controls/CompactComboBox.qml"
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/controls/CompactDelegate.qml"
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/controls/CompactField.qml"
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/controls/CompactGroupBox.qml"
        "${QT_PRESENTATION_RESOURCE_DIR}/qml/controls/CompactRadioButton.qml")
