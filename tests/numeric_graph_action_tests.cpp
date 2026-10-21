#include "synthetic_capture.h"
#include "editor_ui_test_fixture.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/workspace/workspace_window.h>
#include <desktop/numeric_project_document.h>
#include <barelief/ui_text.h>
#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QMenu>
#include <QAction>
#include <QAbstractButton>
#include <QtTest/QSignalSpy>
#include <cstdio>
#include <stdexcept>
static void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
using editor_ui_test_fixture::wait_until;
static QList<WorkspaceDock*> docks(int node){QList<WorkspaceDock*> result;const auto widgets=QApplication::allWidgets();for(auto* widget:widgets)if(auto* dock=qobject_cast<WorkspaceDock*>(widget);dock && dock->property("graphNode").isValid() && dock->property("graphNode").toInt()==node)result.append(dock);return result;}
static QtNodes::NodeGraphicsObject* owning_node(QGraphicsItem* hit){
    for(auto* parent=hit;parent;parent=parent->parentItem())
        if(auto* node=qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(parent))return node;
    return nullptr;
}
static void require_menu_target(QtNodes::NodeGraphicsObject* item,QtNodes::NodeGraphicsObject* owner,
                                int node,QPointF point){
    if(owner->nodeId()==static_cast<QtNodes::NodeId>(node))return;
    std::fprintf(stderr,"Menu hit desired=%d actual=%u desiredPos=(%.1f,%.1f) hitPos=(%.1f,%.1f) point=(%.1f,%.1f) desiredBounds=(%.1f,%.1f)\n",node,owner->nodeId(),item->pos().x(),item->pos().y(),owner->pos().x(),owner->pos().y(),point.x(),point.y(),item->boundingRect().width(),item->boundingRect().height());
    throw std::runtime_error("Menu precondition: target node overlapped");
}
static QMenu* node_menu(QtNodes::DataFlowGraphicsScene* scene,int node){
    auto* item=scene->nodeGraphicsObject(node);require(item,"Scene node missing");
    const auto point=item->sceneBoundingRect().center();
    const auto items=scene->items(point);
    for(auto* hit:items)if(auto* owner=owning_node(hit)){
        require_menu_target(item,owner,node,point);break;
    }
    return scene->createSceneMenu(point);
}
static void report_missing_dock(int node,const QString& name,const QAction& action,qsizetype before){
    std::fprintf(stderr,"Action node=%d name=%s label=%s before=%lld after=%lld\n",node,qPrintable(name),qPrintable(action.text()),static_cast<long long>(before),static_cast<long long>(docks(node).size()));
    const auto widgets=QApplication::allWidgets();for(auto* widget:widgets)if(auto* dock=qobject_cast<WorkspaceDock*>(widget);dock && dock->property("graphNode").isValid())
        std::fprintf(stderr,"Dock graphNode=%d closed=%d floating=%d\n",dock->property("graphNode").toInt(),dock->isClosed(),dock->isFloating());
}
static WorkspaceDock* trigger(QtNodes::DataFlowGraphicsScene* scene,int node,const QString& name){
    const auto before=docks(node);std::unique_ptr<QMenu> menu(node_menu(scene,node));auto* action=menu->findChild<QAction*>(name);require(action && action->isEnabled(),"Requested graph QAction absent/disabled");action->trigger();
    if(!wait_until([&]{return docks(node).size()==before.size()+1;})){
        report_missing_dock(node,name,*action,before.size());
        throw std::runtime_error("Actual QAction failed to create exactly one graph representation");
    }
    WorkspaceDock* created=nullptr;const auto current=docks(node);for(auto* dock:current)if(!before.contains(dock))created=dock;require(created && !created->isClosed(),"New representation remains closed");return created;
}
static domain::NumericProcessingGraph graph_with_catalog_sinks(){
    auto graph=numeric_default_graph();int next=20;
    const auto kinds=numeric_editor::catalog();
    for(const auto& kind:kinds){
        auto configured=numeric_editor::default_node(kind);domain::ResolvedProcessingNode resolved;
        if(!domain::resolve_processing_node(configured,numeric_editor::limits(),resolved) || resolved.role!=domain::NodeRole::Sink)continue;
        bool present=false;for(const auto& node:graph.nodes)present|=node.kind==configured.kind;
        if(!present){configured.id=next++;graph.nodes.push_back(std::move(configured));}
    }
    // Include catalog-added scalar views as well as the factory metrics view.
    for(auto& node:graph.nodes)if(node.kind=="ScalarView" || node.kind=="ScalarMetricsView"){
        require(node.inputs.size()==1,"Scalar view fixture has unexpected inputs");
        auto second=node.inputs.front();second.id=1;node.inputs.push_back(second);
    }
    return graph;
}
static desktop::NumericDraftPresentation graph_settings(const domain::NumericProcessingGraph& graph){
    desktop::NumericDraftPresentation settings;
    // This menu/endpoint scenario opens every catalog view; rendering cadence is independent of its running DSP.
    settings.application_settings={{"ui",QJsonObject{{"autosave_enabled",false},{"pipeline_auto_apply",false},{"graph_rendering",QJsonObject{{"gui_fps_mode","fixed"},{"gui_target_fps",1}}},{"language","en"},{"theme","light"},{"show_data_flow",true},{"show_raw_samples",false},{"show_amplitudes",false},{"show_test_amplitudes",false},{"show_spectrogram",false}}}};
    int position=0;for(const auto& node:graph.nodes){settings.positions[QString::number(node.id)]=QJsonObject{{"x",(position%4)*10000},{"y",(position/4)*10000}};++position;}
    return settings;
}
static void verify_single_input_menu(QtNodes::DataFlowGraphicsScene* scene,int node){
    std::unique_ptr<QMenu> english(node_menu(scene,node));
    require(!english->findChild<QAction*>("openGraphPort0") &&
            !english->findChild<QAction*>("openGraphPort65526"),
            "Single data input or parameter port leaked into English view menu");
    require(english->findChild<QAction*>("openGraphPlacement1"),"Graph placement missing from English view menu");
    require(ui::Language::instance().set("ru"),"Russian UI translation unavailable");
    std::unique_ptr<QMenu> russian(node_menu(scene,node));
    require(!russian->findChild<QAction*>("openGraphPort0") &&
            !russian->findChild<QAction*>("openGraphPort65526"),
            "Single data input or parameter port leaked into Russian view menu");
    require(russian->findChild<QAction*>("openGraphPlacement1"),"Graph placement missing from Russian view menu");
    require(ui::Language::instance().set("en"),"English UI translation unavailable");
}
static void verify_multi_input_menu(QtNodes::DataFlowGraphicsScene* scene,int node,CoreApplication& app){
    std::unique_ptr<QMenu> menu(node_menu(scene,node));
    const auto* first=menu->findChild<QAction*>("openGraphPort0");
    const auto* second=menu->findChild<QAction*>("openGraphPort1");
    require(first && second && first->text()=="Input port 1" && second->text()=="Input port 2",
            "Multi-input view lost its distinct data port choices");
    auto* dock=trigger(scene,node,"openGraphPort1");
    const auto key=dock->property("graphViewKey").toString();
    require(!key.isEmpty() && app._ctx.file_configs.ui.graph_views.value(key).toObject().value("port").toInt(-1)==1,
            "Selected data port was not preserved in the new view settings");
}
static void open_sink_views(QtNodes::DataFlowGraphicsScene* scene,const domain::NumericProcessingGraph& graph,
                            MainWindow* window,CoreApplication& app,QList<WorkspaceDock*>& requested){
    auto* fixed=window->workspaceDock("spectrogramDock");
    require(fixed && fixed->isClosed(),"All-closed graph fixture lost its closed fixed tab");
    for(const auto& node:graph.nodes){
        domain::ResolvedProcessingNode resolved;
        require(bool(domain::resolve_processing_node(node,numeric_editor::limits(),resolved)),"Fixture node contract invalid");
        if(!numeric_editor::is_graph_view(QString::fromStdString(node.kind)))continue;
        auto* dock=trigger(scene,node.id,"openGraphPlacement1");requested.append(dock);
        require(window->tabifiedDockWidgets(fixed).contains(dock) && fixed->isClosed(),
                "Open in new tab detached from the existing group or reopened a closed graph");
        require(!dock->property("numericEndpoint").toMap().isEmpty(),"Working view has no runtime endpoint");
        if(node.kind=="FFTView")verify_single_input_menu(scene,node.id);
        if(node.kind=="ScalarView" || node.kind=="ScalarMetricsView")verify_multi_input_menu(scene,node.id,app);
    }
}
static WorkspaceDock* open_placement_variants(QtNodes::DataFlowGraphicsScene* scene,QList<WorkspaceDock*>& requested){
    auto* source=trigger(scene,9,"openGraphPlacement1");requested.append(source);
    requested.append(trigger(scene,10,"openGraphPlacement1"));
    auto* sourceAgain=trigger(scene,9,"openGraphPlacement1");requested.append(sourceAgain);
    require(sourceAgain!=source && !source->isClosed(),"New tab reused or destroyed existing representation");
    auto* here=trigger(scene,9,"openGraphPlacement2");requested.append(here);
    require(!here->isFloating(),"Open here unexpectedly floated graph");
    auto* external=trigger(scene,9,"openGraphPlacement0");requested.append(external);
    require(external->isFloating() && !here->isClosed(),"External action failed or moved existing internal graph");
    return external;
}
static void open_split_and_selected(QtNodes::DataFlowGraphicsScene* scene,SchemeEditorWidget* editor,
                                    QSignalSpy& opened){
    std::unique_ptr<QMenu> splitMenu(node_menu(scene,10));auto* beside=splitMenu->findChild<QMenu*>("graphPlacementTargets");
    require(beside && beside->isEnabled() && !beside->actions().isEmpty(),"Split target menu unavailable");
    const auto beforeSplit=docks(10).size();const auto actions=beside->actions();actions.first()->trigger();
    require(wait_until([&]{return docks(10).size()==beforeSplit+1;}),"Actual split QAction did not create representation");
    scene->clearSelection();scene->nodeGraphicsObject(9)->setSelected(true);
    auto* selected=editor->findChild<QAction*>("showSelectedGraph");require(selected,"Selected graph action missing");
    const auto beforeSelected=docks(9).size();selected->trigger();
    require(wait_until([&]{return docks(9).size()==beforeSelected+1;}) && !opened.isEmpty() &&
        opened.last()[0].toInt()==9 && opened.last()[1].toInt()==1,"Selected source QAction lost node/placement routing");
}
static void apply_explicitly(CoreApplication& app,MainWindow* window,QSignalSpy& applied,quint64 initialRevision){
    const auto preApply=app.presenter().client().snapshot();
    std::fprintf(stderr,"Before explicit Apply: signals=%lld graphEmpty=%d analyzerPresent=%d analyzerState=%d autoApply=%d\n",static_cast<long long>(applied.size()),preApply.graph.isEmpty(),preApply.statuses.contains("analyzer"),static_cast<int>(preApply.statuses.value("analyzer").state),app._ctx.file_configs.ui.pipeline_auto_apply);
    require(applied.isEmpty() && preApply.graph_revision==initialRevision && preApply.statuses.value("analyzer").state==pipeline::State::Running,"Graph opening changed the working core");
    auto* button=window->findChild<QAbstractButton*>("applyPipeline");require(button,"Explicit Apply button missing");
    button->click();require(wait_until([&]{return !applied.isEmpty();}) && applied.last()[1].toBool(),"Explicit Apply failed");
    require(app.presenter().client().snapshot().statuses.value("analyzer").state==pipeline::State::Running,"Explicit Apply did not keep the core active");
}
static void verify_dock_bindings(const QList<WorkspaceDock*>& requested,const domain::NumericProcessingGraph& graph,
                                 CoreApplication& app){
    for(auto* dock:requested){
        const auto binding=dock->property("numericEndpoint").toMap();const auto id=dock->property("graphNode").toInt();
        const domain::ConfiguredProcessingNode* configured=nullptr;
        for(const auto& node:graph.nodes)if(node.id==id)configured=&node;
        require(configured,"Dock node ID drifted");domain::ResolvedProcessingNode resolved;
        require(bool(domain::resolve_processing_node(*configured,numeric_editor::limits(),resolved)),"Bound node invalid");
        require(binding["node"].toInt()==id && binding["graph"].toString()==QString::fromStdString(graph.graph_id) &&
            binding["device"].toString()==app.presenter().device_id() && binding["port"].toInt()==0 &&
            binding["direction"].toString()==(resolved.role==domain::NodeRole::Sink?"input":"output"),"Actual runtime binding has wrong endpoint semantic/identity");
    }
}
static void verify_floating_editor(MainWindow* window,SchemeEditorWidget* editor,
                                   QtNodes::DataFlowGraphicsScene* scene,WorkspaceDock* external){
    WorkspaceDock* flow=nullptr;
    const auto windowDocks=window->findChildren<WorkspaceDock*>();
    for(auto* dock:windowDocks)if(dock->widget()==editor)flow=dock;
    require(flow,"Data Flow dock missing");flow->setFloating(true);
    require(wait_until([&]{return flow->isFloating();}),"Isolated Data Flow did not float");
    auto* scoped=trigger(scene,9,"openGraphPlacement1");
    require(scoped->sharesHost(flow),"Opening from floating editor targeted another pipeline host");
    require(!external->isClosed() && external->isFloating(),"Floating editor open stole earlier external representation");
}
static void run_action_scenario(CoreApplication& app,const domain::NumericProcessingGraph& graph){
    app._ctx.file_configs.ui.autosave_enabled=false;
    auto* window=editor_ui_test_fixture::main_window(require);window->show();
    auto* editor=editor_ui_test_fixture::editor(*window,require);
    auto* scene=editor_ui_test_fixture::scene(*editor,require);
    require(wait_until([&]{return editor->isVisible() && app.presenter().client().snapshot().graph_revision>0 &&
        !app.presenter().graph_request_pending();}),"Initial continuous graph unavailable");
    const auto initialRevision=app.presenter().client().snapshot().graph_revision;
    QSignalSpy applied(&app.presenter(),&GuiPresenter::graph_apply_finished);
    QSignalSpy opened(editor,&SchemeEditorWidget::graph_open_requested);
    QList<WorkspaceDock*> requested;
    open_sink_views(scene,graph,window,app,requested);
    auto* external=open_placement_variants(scene,requested);
    open_split_and_selected(scene,editor,opened);
    apply_explicitly(app,window,applied,initialRevision);
    verify_dock_bindings(requested,graph,app);
    verify_floating_editor(window,editor,scene,external);
}
int main(int argc,char** argv){
    if(qEnvironmentVariable("QT_QPA_PLATFORM")!="offscreen"){
        std::fprintf(stderr,"This fixture requires explicit QT_QPA_PLATFORM=offscreen\n");
        return 2;
    }
    try {
        QTemporaryDir directory;require(directory.isValid(),"Isolated directory unavailable");
        const auto path=directory.filePath("project.json");
        const auto graph=graph_with_catalog_sinks();
        require(editor_ui_test_fixture::save_project(path,graph,graph_settings(graph),require),
                "Isolated saved graph write failed");
        auto encoded=path.toLocal8Bit();
        char executable[]="numeric_graph_action_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory(false));
        require(QGuiApplication::platformName()=="offscreen","This fixture must only run with offscreen platform");
        run_action_scenario(app,graph);
        std::puts("actual graph QAction: saved unapplied sinks/source/processors, repeated tabs/here/window/split, endpoint bindings and floating editor scope PASS");
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
