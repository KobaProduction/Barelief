#include "synthetic_capture.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/workspace/workspace_window.h>
#include <desktop/numeric_project_document.h>
#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QMenu>
#include <QAction>
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QThread>
#include <QtTest/QSignalSpy>
#include <cstdio>
#include <stdexcept>
#include <functional>
static void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
static bool wait_until(const std::function<bool()>& test){QElapsedTimer clock;clock.start();while(clock.elapsed()<2500){QCoreApplication::processEvents();if(test())return true;QThread::msleep(5);}return test();}
static QList<WorkspaceDock*> docks(int node){QList<WorkspaceDock*> result;for(auto* widget:QApplication::allWidgets())if(auto* dock=qobject_cast<WorkspaceDock*>(widget);dock && dock->property("graphNode").isValid() && dock->property("graphNode").toInt()==node)result.append(dock);return result;}
static QMenu* node_menu(QtNodes::DataFlowGraphicsScene* scene,int node){auto* item=scene->nodeGraphicsObject(node);require(item,"Scene node missing");const auto point=item->sceneBoundingRect().center();
    for(auto* hit:scene->items(point)){QtNodes::NodeGraphicsObject* owner=nullptr;for(auto* parent=hit;parent;parent=parent->parentItem())if((owner=qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(parent)))break;if(!owner)continue;
        if(owner->nodeId()!=static_cast<QtNodes::NodeId>(node)){std::fprintf(stderr,"Menu hit desired=%d actual=%u desiredPos=(%.1f,%.1f) hitPos=(%.1f,%.1f) point=(%.1f,%.1f) desiredBounds=(%.1f,%.1f)\n",node,owner->nodeId(),item->pos().x(),item->pos().y(),owner->pos().x(),owner->pos().y(),point.x(),point.y(),item->boundingRect().width(),item->boundingRect().height());throw std::runtime_error("Menu precondition: target node overlapped");}break;}
    return scene->createSceneMenu(point);}
static WorkspaceDock* trigger(QtNodes::DataFlowGraphicsScene* scene,int node,const QString& name){
    const auto before=docks(node);std::unique_ptr<QMenu> menu(node_menu(scene,node));auto* action=menu->findChild<QAction*>(name);require(action && action->isEnabled(),"Requested graph QAction absent/disabled");action->trigger();
    if(!wait_until([&]{return docks(node).size()==before.size()+1;})){
        std::fprintf(stderr,"Action node=%d name=%s label=%s before=%lld after=%lld\n",node,qPrintable(name),qPrintable(action->text()),static_cast<long long>(before.size()),static_cast<long long>(docks(node).size()));
        for(auto* widget:QApplication::allWidgets())if(auto* dock=qobject_cast<WorkspaceDock*>(widget);dock && dock->property("graphNode").isValid())std::fprintf(stderr,"Dock graphNode=%d closed=%d floating=%d\n",dock->property("graphNode").toInt(),dock->isClosed(),dock->isFloating());
        throw std::runtime_error("Actual QAction failed to create exactly one graph representation");
    }
    WorkspaceDock* created=nullptr;for(auto* dock:docks(node))if(!before.contains(dock))created=dock;require(created && !created->isClosed(),"New representation remains closed");return created;
}
int main(int argc,char** argv){
    if(qEnvironmentVariable("QT_QPA_PLATFORM")!="offscreen"){std::fprintf(stderr,"This fixture requires explicit QT_QPA_PLATFORM=offscreen\n");return 2;}
    try {
        QTemporaryDir directory;require(directory.isValid(),"Isolated directory unavailable");const auto path=directory.filePath("project.json");
        auto graph=numeric_test_graph();int next=20;
        for(const auto& kind:numeric_editor::catalog()){
            auto configured=numeric_editor::default_node(kind);domain::ResolvedProcessingNode resolved;
            if(!domain::resolve_processing_node(configured,numeric_editor::limits(),resolved) || resolved.role!=domain::NodeRole::Sink)continue;
            bool present=false;for(const auto& node:graph.nodes)present|=node.kind==configured.kind;
            if(!present){configured.id=next++;graph.nodes.push_back(std::move(configured));}
        }
        desktop::NumericDraftPresentation settings;settings.application_settings={{"ui",QJsonObject{{"autosave_enabled",false},{"pipeline_auto_apply",false},{"language","en"},{"theme","light"},{"show_data_flow",true},{"show_raw_samples",false},{"show_amplitudes",false},{"show_test_amplitudes",false},{"show_spectrogram",false}}}};
        int position=0;for(const auto& node:graph.nodes){settings.positions[QString::number(node.id)]=QJsonObject{{"x",(position%4)*10000},{"y",(position/4)*10000}};++position;}
        desktop::NumericProjectDocument document;require(bool(desktop::create_numeric_project(graph,settings,document)),"Canonical saved graph creation failed");require(bool(desktop::save_numeric_project(document,path)),"Isolated saved graph write failed");
        auto encoded=path.toLocal8Bit();char executable[]="numeric_graph_action_tests",option[]="--config";char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory(false));require(QGuiApplication::platformName()=="offscreen","This fixture must only run with offscreen platform");
        app._ctx.file_configs.ui.autosave_enabled=false;
        MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;require(window,"Application MainWindow missing");window->show();
        auto* editor=window->findChild<SchemeEditorWidget*>();require(editor,"Application editor missing");auto* scene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();require(scene,"Editor scene missing");
        require(wait_until([&]{return editor->isVisible() && app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();}),"Initial continuous graph unavailable");
        const auto initialRevision=app.presenter().client().snapshot().graph_revision;
        QSignalSpy applied(&app.presenter(),&GuiPresenter::graph_apply_finished),opened(editor,&SchemeEditorWidget::graph_open_requested);
        QList<WorkspaceDock*> requested;
        for(const auto& node:graph.nodes){domain::ResolvedProcessingNode resolved;require(bool(domain::resolve_processing_node(node,numeric_editor::limits(),resolved)),"Fixture node contract invalid");if(!numeric_editor::is_graph_view(QString::fromStdString(node.kind)))continue;
            auto* dock=trigger(scene,node.id,"openGraphPlacement1");requested.append(dock);require(!dock->property("numericEndpoint").toMap().isEmpty(),"Working view has no runtime endpoint");
            std::unique_ptr<QMenu> menu(node_menu(scene,node.id));auto* port=menu->findChild<QAction*>("openGraphPort0");require(port && port->text().contains("Input port"),"Sink menu chooses output instead of input port");
        }
        auto* source=trigger(scene,9,"openGraphPlacement1");requested.append(source);
        auto* processor=trigger(scene,10,"openGraphPlacement1");requested.append(processor);
        auto* sourceAgain=trigger(scene,9,"openGraphPlacement1");requested.append(sourceAgain);require(sourceAgain!=source && !source->isClosed(),"New tab reused or destroyed existing representation");
        auto* here=trigger(scene,9,"openGraphPlacement2");requested.append(here);require(!here->isFloating(),"Open here unexpectedly floated graph");
        auto* external=trigger(scene,9,"openGraphPlacement0");requested.append(external);require(external->isFloating() && !here->isClosed(),"External action failed or moved existing internal graph");
        std::unique_ptr<QMenu> splitMenu(node_menu(scene,10));auto* beside=splitMenu->findChild<QMenu*>("graphPlacementTargets");require(beside && beside->isEnabled() && !beside->actions().isEmpty(),"Split target menu unavailable");
        const auto beforeSplit=docks(10).size();beside->actions().first()->trigger();require(wait_until([&]{return docks(10).size()==beforeSplit+1;}),"Actual split QAction did not create representation");
        scene->clearSelection();scene->nodeGraphicsObject(9)->setSelected(true);auto* selected=editor->findChild<QAction*>("showSelectedGraph");require(selected,"Selected graph action missing");const auto beforeSelected=docks(9).size();selected->trigger();
        require(wait_until([&]{return docks(9).size()==beforeSelected+1;}) && !opened.isEmpty() && opened.last()[0].toInt()==9 && opened.last()[1].toInt()==1,"Selected source QAction lost node/placement routing");
        const auto preApply=app.presenter().client().snapshot();
        std::fprintf(stderr,"Before explicit Apply: signals=%lld graphEmpty=%d analyzerPresent=%d analyzerState=%d autoApply=%d\n",static_cast<long long>(applied.size()),preApply.graph.isEmpty(),preApply.statuses.contains("analyzer"),static_cast<int>(preApply.statuses.value("analyzer").state),app._ctx.file_configs.ui.pipeline_auto_apply);
        require(applied.isEmpty() && preApply.graph_revision==initialRevision && preApply.statuses.value("analyzer").state==pipeline::State::Running,"Graph opening changed the working core");
        auto* applyButton=window->findChild<QAbstractButton*>("applyPipeline");require(applyButton,"Explicit Apply button missing");applyButton->click();require(wait_until([&]{return !applied.isEmpty();}) && applied.last()[1].toBool(),"Explicit Apply failed");
        require(app.presenter().client().snapshot().statuses.value("analyzer").state==pipeline::State::Running,"Explicit Apply did not keep the core active");
        for(auto* dock:requested){const auto binding=dock->property("numericEndpoint").toMap();const auto id=dock->property("graphNode").toInt();const domain::ConfiguredProcessingNode* configured=nullptr;for(const auto& node:graph.nodes)if(node.id==id)configured=&node;require(configured,"Dock node ID drifted");domain::ResolvedProcessingNode resolved;require(bool(domain::resolve_processing_node(*configured,numeric_editor::limits(),resolved)),"Bound node invalid");
            require(binding["node"].toInt()==id && binding["graph"].toString()==QString::fromStdString(graph.graph_id) && binding["device"].toString()==app.presenter().device_id() && binding["port"].toInt()==0 && binding["direction"].toString()==(resolved.role==domain::NodeRole::Sink?"input":"output"),"Actual runtime binding has wrong endpoint semantic/identity");
        }
        WorkspaceDock* flow=nullptr;for(auto* dock:window->findChildren<WorkspaceDock*>())if(dock->widget()==editor)flow=dock;require(flow,"Data Flow dock missing");flow->setFloating(true);require(wait_until([&]{return flow->isFloating();}),"Isolated Data Flow did not float");
        auto* scoped=trigger(scene,9,"openGraphPlacement1");require(scoped->sharesHost(flow),"Opening from floating editor targeted another pipeline host");
        require(!external->isClosed() && external->isFloating(),"Floating editor open stole earlier external representation");
        std::puts("actual graph QAction: saved unapplied sinks/source/processors, repeated tabs/here/window/split, endpoint bindings and floating editor scope PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
