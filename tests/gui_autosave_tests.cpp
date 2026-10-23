#include "isolated_gui_platform.h"
#include "synthetic_capture.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/numeric_node_operations.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <QFileInfo>
#include <QLabel>
#include <QDebug>
#include <config/config_document.h>
#include <QFile>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonDocument>
#include <cstdio>

static bool require(bool okay,const char* message) {
    if(!okay)std::fprintf(stderr,"FAIL: %s\n",message);
    return okay;
}
static QByteArray read(const QString& path) {
    QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
}
static bool wait_until(const auto& predicate) {
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<10000) {
        QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(2);
    }
    return false;
}
static bool seed(const QString& path) {
    desktop::NumericProjectDocument document;
    const auto source=QByteArray("{\"schema_version\":4,\"opaque\":{\"exact\":1e9999,\"payload\":\"")+QByteArray(800000,'a')+"\"}}";
    if(!desktop::read_numeric_project(source,document))return false;
    domain::NumericProcessingGraph graph;graph.graph_id="main";
    auto node=numeric_editor::default_node("Converter");node.id=7;graph.nodes.push_back(node);
    desktop::NumericDraftPresentation display;
    display.application_settings={{"ui",QJsonObject{{"pipeline_auto_apply",false},{"show_spectrogram",false}}}};
    return desktop::stage_numeric_draft(document,graph,document) &&
        desktop::stage_numeric_presentation(document,display,document) && desktop::save_numeric_project(document,path);
}
static MainWindow* main_window() {
    for(auto* widget:QApplication::topLevelWidgets())if(auto* window=qobject_cast<MainWindow*>(widget))return window;
    return nullptr;
}
static bool saved(MainWindow* window) {
    auto* status=window->findChild<QLabel*>("configurationSaveStatus");
    return status && status->property("saveState").toInt()==int(MainWindow::ConfigurationSaveState::Saved);
}
static bool edit(CoreApplication& app,MainWindow* window,double x) {
    auto* editor=window->findChild<SchemeEditorWidget*>();
    auto* graph=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
    if(!editor || !graph)return false;
    graph->setNodeData(7,QtNodes::NodeRole::Position,QPointF(x,88));
    app._ctx.editor_draft_modified=true;emit editor->numeric_draft_changed();return true;
}
static bool position(const QString& path,double x) {
    desktop::NumericProjectDocument document;domain::NumericProcessingGraph graph;
    desktop::NumericDraftPresentation presentation;
    return desktop::load_numeric_project(path,document,graph) && desktop::read_numeric_presentation(document,presentation)
        && presentation.positions.value("7").toObject().value("x").toDouble()==x;
}
static bool parameter_event(CoreApplication& app,MainWindow* window,const QString& path) {
    auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
    auto* node=model?model->delegateModel<AudioNodeModel>(7):nullptr;if(!node || !node->numeric_node())return false;
    auto settings=*node->numeric_node();settings.enabled=false;node->set_numeric_node(settings);
    if(!wait_until([&]{return saved(window);}))return require(false,"node settings event was not saved");
    desktop::NumericProjectDocument document;domain::NumericProcessingGraph graph;
    return require(desktop::load_numeric_project(path,document,graph) && !graph.nodes.front().enabled,"node settings snapshot was lost");
}
static void pump_events(int milliseconds) {
    QElapsedTimer idle;idle.start();
    while(idle.elapsed()<milliseconds){QCoreApplication::processEvents();QThread::msleep(2);}
}
static QJsonObject saved_settings(const QByteArray& source) {
    desktop::NumericProjectDocument document;QJsonObject settings;
    if(desktop::read_numeric_project(source,document))desktop::read_numeric_application_settings(document,settings);
    return settings;
}
static bool navigation_does_not_save(CoreApplication& app,MainWindow* window,const QString& path) {
    auto* surface=window->findChild<SpectrogramGraph*>();if(!surface)return false;
    // Saved node settings do not imply the first dock-layout timer has fired.
    // Wait for the actual persisted workspace, not an arbitrary startup delay.
    auto modified=QFileInfo(path).lastModified();
    auto workspace=saved_settings(read(path)).value("ui").toObject().value("workspace").toObject();
    if(!wait_until([&]{
        const auto current=QFileInfo(path).lastModified();
        if(current!=modified){modified=current;workspace=saved_settings(read(path)).value("ui").toObject().value("workspace").toObject();}
        return saved(window) && workspace.value("version").toInt()==1;
    }))return require(false,"initial dock layout was not persisted");
    const auto before=read(path);const auto baselineModified=QFileInfo(path).lastModified();
    const QJsonObject camera{{"rotation_x",17},{"rotation_y",29},{"zoom",1.25}};
    QObject diagnostics;
    QObject::connect(window,&MainWindow::settings_change_requested,&diagnostics,[](const ui::SettingsChangeRequest& request){
        qWarning()<<"NAVIGATION_SAVE_REQUEST scope="<<int(request.scope)<<" force="<<request.force_save;
    });
    emit surface->camera_changed(camera);
    pump_events(2200);
    if(read(path)!=before)qWarning().noquote()<<"NAVIGATION_SAVE_DELTA"<<QJsonDocument(configuration::changed_fields(
        saved_settings(before),saved_settings(read(path)))).toJson(QJsonDocument::Compact);
    return require(saved(window) && read(path)==before && QFileInfo(path).lastModified()==baselineModified,
        "camera navigation/idle rewrote the project or marked it dirty") &&
        require(app._ctx.file_configs.ui.camera==camera,"navigation state was not retained in memory");
}
static bool disabled_save_and_close(CoreApplication& app,MainWindow* window,const QString& path) {
    app._ctx.file_configs.ui.autosave_enabled=false;
    const auto disabled=read(path);if(!edit(app,window,456))return false;
    for(int i=0;i<30;++i){QCoreApplication::processEvents();QThread::msleep(2);}
    if(!require(read(path)==disabled,"disabled autosave wrote the project"))return false;
    app.save_configuration_as(path);
    if(!require(position(path,456),"manual Save failed with autosave disabled"))return false;
    desktop::NumericProjectDocument document;QJsonObject settings;
    if(!require(desktop::read_numeric_project(read(path),document) &&
        desktop::read_numeric_application_settings(document,settings) &&
        settings.value("ui").toObject().value("camera").toObject()==app._ctx.file_configs.ui.camera,
        "manual Save did not persist the remembered camera"))return false;
    app._ctx.file_configs.ui.autosave_enabled=true;
    if(!edit(app,window,789))return false;
    QCoreApplication::processEvents();window->close();return true;
}
static bool exercise(CoreApplication& app,const QString& path) {
    auto* window=main_window();if(!require(window,"window unavailable"))return false;
    window->setAttribute(Qt::WA_ShowWithoutActivating);
    window->setWindowFlag(Qt::WindowDoesNotAcceptFocus);window->show();
    if(!require(app._ctx.file_configs.ui.autosave_enabled,"autosave default disabled"))return false;
    if(!require(wait_until([&]{return !app.presenter().graph_request_pending();}),"startup did not settle"))return false;
    const auto before=read(path);
    if(!require(edit(app,window,99),"first edit failed"))return false;
    if(!require(read(path)==before,"autosave wrote synchronously inside the edit callback"))return false;
    QCoreApplication::processEvents();
    // Capture another edit while the first immutable snapshot is in flight.
    if(!require(edit(app,window,123),"concurrent edit failed"))return false;
    if(!wait_until([&]{return saved(window);})) {
        auto* status=window->findChild<QLabel*>("configurationSaveStatus");
        std::fprintf(stderr,"save status: %s / %s\n",qPrintable(status?status->text():QString{}),qPrintable(status?status->toolTip():QString{}));
        return require(false,"background saves did not complete");
    }
    if(!require(position(path,123),"newer edit was dropped by completion of the older snapshot"))return false;
    if(!require(read(path).contains("1e9999"),"opaque source lexeme changed"))return false;
    if(!parameter_event(app,window,path))return false;
    if(!navigation_does_not_save(app,window,path))return false;
    return disabled_save_and_close(app,window,path);
}
static bool external_change(CoreApplication& app,const QString& path) {
    auto* window=main_window();if(!window || !edit(app,window,999))return false;
    QFile file(path);if(!file.open(QIODevice::Append) || file.write("\n")!=1)return false;file.close();
    const auto changed=read(path);
    const bool refused=wait_until([&]{auto* status=window->findChild<QLabel*>("configurationSaveStatus");
        return status && status->property("saveState").toInt()==int(MainWindow::ConfigurationSaveState::Error);});
    app._ctx.file_configs.ui.autosave_enabled=false;
    return require(refused && read(path)==changed,"background save overwrote an externally changed project");
}
int main(int argc,char** argv) {
    const bool conflict=argc==2 && QByteArray(argv[1])=="--conflict";
    if(!test_fixture::isolated_gui_platform())return 1;
    QTemporaryDir directory;const auto path=directory.filePath("project.json");
    if(!require(seed(path),"seed failed"))return 1;
    {
        QByteArray executable="gui_autosave_tests",option="--config",file=path.toUtf8();
        char* arguments[]{executable.data(),option.data(),file.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        if(!(conflict?external_change(app,path):exercise(app,path)))return 1;
    }
    if(!conflict && !require(position(path,789),"closing lost the final edit"))return 1;
    std::puts("background autosave: concurrent edits, disable, manual save, close flush and opaque source PASS");return 0;
}
