#pragma once

#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <desktop/numeric_project_document.h>
#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <functional>

namespace editor_ui_test_fixture {
using Check = void (*)(bool, const char*);

inline bool save_project(const QString& path,
                         const domain::NumericProcessingGraph& graph,
                         const desktop::NumericDraftPresentation& presentation,
                         Check check) {
    desktop::NumericProjectDocument document;
    const bool created=bool(desktop::create_numeric_project(graph,presentation,document));
    check(created,"Create canonical editor project");
    if(!created)return false;
    const bool saved=bool(desktop::save_numeric_project(document,path));
    check(saved,"Save isolated editor project");
    return saved;
}

inline MainWindow* main_window(Check check) {
    const auto widgets=QApplication::topLevelWidgets();
    for(auto* widget:widgets)
        if(auto* window=qobject_cast<MainWindow*>(widget))return window;
    check(false,"Application MainWindow missing");
    return nullptr;
}

inline SchemeEditorWidget* editor(MainWindow& window,Check check) {
    auto* result=window.findChild<SchemeEditorWidget*>();
    check(result!=nullptr,"Application editor missing");
    return result;
}

inline QtNodes::DataFlowGraphicsScene* scene(SchemeEditorWidget& editor,Check check) {
    auto* result=editor.findChild<QtNodes::DataFlowGraphicsScene*>();
    check(result!=nullptr,"Editor scene missing");
    return result;
}

inline QtNodes::GraphicsView* view(SchemeEditorWidget& editor,Check check) {
    auto* result=editor.findChild<QtNodes::GraphicsView*>();
    check(result!=nullptr,"Editor view missing");
    return result;
}

inline bool wait_until(const std::function<bool()>& ready,int timeout_ms=2500) {
    QElapsedTimer timer;
    timer.start();
    while(timer.elapsed()<timeout_ms) {
        QCoreApplication::processEvents();
        if(ready())return true;
        QThread::msleep(2);
    }
    return ready();
}
} // namespace editor_ui_test_fixture
