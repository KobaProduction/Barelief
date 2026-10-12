#include "synthetic_capture.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <desktop/numeric_project_document.h>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QAbstractButton>
#include <QAction>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QLineEdit>
#include <QComboBox>
#include <QLineF>
#include <cmath>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QThread>
#include <QtTest/QSignalSpy>
#include <cstdio>
#include <stdexcept>
#include <functional>
#include <algorithm>
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static bool wait_until(const std::function<bool()>& predicate){QElapsedTimer clock;clock.start();while(clock.elapsed()<2500){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(5);}return predicate();}
static void trigger(QWidget* root,const char* name){
    if(QString::fromLatin1(name)=="undoDataFlow" || QString::fromLatin1(name)=="redoDataFlow") {
        root->activateWindow(); auto* canvas=root->findChild<QtNodes::GraphicsView*>();
        require(canvas,"history editor context"); canvas->setFocus(); QCoreApplication::processEvents();
    }
    auto* action=root->findChild<QAction*>(name);require(action && action->isEnabled(),name);action->trigger();QCoreApplication::processEvents();
}
static QAction* shortcut(QtNodes::GraphicsView* view,QKeySequence::StandardKey key){for(auto* action:view->actions())if(action->shortcuts().contains(QKeySequence(key)))return action;throw std::runtime_error("Canvas action missing");}
int main(int argc,char** argv){
    if(qEnvironmentVariable("QT_QPA_PLATFORM")!="offscreen")return 2;
    try{
        QTemporaryDir temporary;require(temporary.isValid(),"Temporary directory missing");const auto path=temporary.filePath("project.json");
        domain::NumericProcessingGraph empty;empty.graph_id="main";desktop::NumericDraftPresentation presentation;
        presentation.application_settings={{"ui",QJsonObject{{"autosave_enabled",false},{"pipeline_auto_apply",false},{"language","en"},{"show_spectrogram",false},{"show_data_flow",true}}}};
        desktop::NumericProjectDocument document;require(bool(desktop::create_numeric_project(empty,presentation,document)) && bool(desktop::save_numeric_project(document,path)),"Saved canonical fixture creation failed");
        auto encoded=path.toLocal8Bit();char executable[]="numeric_editor_action_tests",option[]="--config";char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory(false));MainWindow* window=nullptr;
        for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;
        require(window,"MainWindow missing");window->show();auto* editor=window->findChild<SchemeEditorWidget*>();require(editor,"Editor missing");
        auto* graph=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());auto* scene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();auto* view=editor->findChild<QtNodes::GraphicsView*>();auto* library=editor->findChild<QTreeWidget*>("pipelineNodeLibrary");require(graph && scene && view && library,"Editor components missing");
        require(wait_until([&]{return editor->isVisible() && app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();}),"Initial graph not applied");
        const auto initialRevision=app.presenter().client().snapshot().graph_revision;QSignalSpy applied(&app.presenter(),&GuiPresenter::graph_apply_finished);
        auto create=[&](const QString& kind){const auto before=graph->allNodeIds();QTreeWidgetItem* entry=nullptr;for(QTreeWidgetItemIterator it(library);*it;++it)if((*it)->data(0,Qt::UserRole).toString()==kind)entry=*it;require(entry,"Library kind missing");emit library->itemDoubleClicked(entry,0);QCoreApplication::processEvents();for(auto id:graph->allNodeIds())if(!before.contains(id))return id;throw std::runtime_error("Library action did not create node");};
        const auto input=create("NetworkInput"),route=create("Switch"),output=create("NetworkOutput"),converter=create("Converter");
        auto* inputModel=graph->delegateModel<AudioNodeModel>(input);std::unique_ptr<QWidget> inputForm(inputModel->parameter_widget());auto* stream=inputForm->findChild<QLineEdit*>("adapterStreamId");require(stream,"Registered network source control missing");
        const auto dspBeforeAdapter=graph->numeric_graph();stream->setText("stream-main");stream->setModified(true);QMetaObject::invokeMethod(stream,"editingFinished",Qt::DirectConnection);require(inputModel->adapter().value("stream_id")=="stream-main","Actual source metadata edit missing");
        trigger(window,"undoDataFlow");require(inputModel->adapter().isEmpty(),"Source metadata Undo failed");trigger(window,"redoDataFlow");require(stream->text()=="stream-main","Source metadata Redo does not sync form");
        QByteArray beforeAdapter,afterAdapter;desktop::encode_numeric_graph(dspBeforeAdapter,beforeAdapter);desktop::encode_numeric_graph(graph->numeric_graph(),afterAdapter);require(beforeAdapter==afterAdapter,"Application adapter changes DSP DTO");
        graph->addConnection({input,0,route,0});graph->addConnection({route,0,output,0});require(graph->connectionExists({input,0,route,0}) && graph->connectionExists({route,0,output,0}),"Typed routes not created");
        auto* model=graph->delegateModel<AudioNodeModel>(converter);std::unique_ptr<QWidget> form(model->parameter_widget());auto* type=form->findChild<QComboBox*>("numericNan_type");auto* literal=form->findChild<QLineEdit*>("numericNan");require(type && literal,"Exact parameter controls missing");type->setCurrentIndex(1);literal->setText("18446744073709551615");literal->setModified(true);QMetaObject::invokeMethod(literal,"editingFinished",Qt::DirectConnection);
        auto nan=[&]{return std::get<domain::ConverterProcessingParameters>(model->numeric_node()->parameters).policy.nan_replacement;};require(std::get<std::uint64_t>(nan())==UINT64_MAX,"Actual edit rounds UInt64");
        trigger(window,"undoDataFlow");require(std::get<std::uint64_t>(nan())==0,"File Undo does not undo parameter edit");trigger(window,"redoDataFlow");require(std::get<std::uint64_t>(nan())==UINT64_MAX,"File Redo loses exact parameter");
        auto* visible=form->findChild<QAction*>("showOnBlock_numericNan");require(visible,"Presentation toggle missing");visible->trigger();
        scene->clearSelection();scene->nodeGraphicsObject(converter)->setSelected(true);const auto beforeCopy=graph->allNodeIds();shortcut(view,QKeySequence::Copy)->trigger();shortcut(view,QKeySequence::Paste)->trigger();QCoreApplication::processEvents();
        QtNodes::NodeId copied=QtNodes::InvalidNodeId;for(auto id:graph->allNodeIds())if(!beforeCopy.contains(id))copied=id;require(copied!=QtNodes::InvalidNodeId && graph->allNodeIds().size()==beforeCopy.size()+1,"Actual Copy/Paste failed");
        auto* copiedModel=graph->delegateModel<AudioNodeModel>(copied);require(domain::same_processing_parameters(model->numeric_node()->parameters,copiedModel->numeric_node()->parameters) && copiedModel->parameter_on_block("numericNan"),"Copy/Paste loses exact parameter or block flag");
        trigger(window,"undoDataFlow");require(!graph->nodeExists(copied),"Paste Undo leaves node");trigger(window,"redoDataFlow");require(graph->nodeExists(copied),"Paste Redo loses node");
        scene->clearSelection();scene->nodeGraphicsObject(route)->setSelected(true);const auto beforeRouteCopy=graph->allNodeIds();shortcut(view,QKeySequence::Copy)->trigger();shortcut(view,QKeySequence::Paste)->trigger();QCoreApplication::processEvents();
        QtNodes::NodeId copiedRoute=QtNodes::InvalidNodeId;for(auto id:graph->allNodeIds())if(!beforeRouteCopy.contains(id))copiedRoute=id;require(copiedRoute!=QtNodes::InvalidNodeId && graph->connections(copiedRoute,QtNodes::PortType::In,0).empty() && graph->connections(copiedRoute,QtNodes::PortType::Out,0).empty(),"Copy partial selection aliases external route IDs");
        scene->clearSelection();scene->nodeGraphicsObject(output)->setSelected(true);view->deleteSelectionAction()->trigger();require(!graph->nodeExists(output) && !graph->connectionExists({route,0,output,0}),"Delete action leaves route");trigger(window,"undoDataFlow");require(graph->nodeExists(output) && graph->connectionExists({route,0,output,0}),"Delete Undo loses route");
        graph->setNodeData(converter,QtNodes::NodeRole::Position,QPointF(1234,5678));view->setupScale(.7);view->centerOn(1300,5700);editor->flush_presentation();const auto savedView=app._ctx.file_configs.ui.editor_view;
        require(editor->save_preset("canonical exact layout"),"Preset save failed");const auto saved=app._ctx.file_configs.ui.pipeline_presets.last().toObject();
        graph->setNodeData(converter,QtNodes::NodeRole::Position,QPointF(-9000,-9000));view->centerOn(0,0);require(editor->load_preset(saved["id"].toString()),"Preset load failed");QCoreApplication::processEvents();require(graph->nodeData(converter,QtNodes::NodeRole::Position).toPointF()==QPointF(1234,5678),"Preset inherits previous graph positions");const auto loadedCenter=view->mapToScene(view->viewport()->rect().center());std::fprintf(stderr,"Preset camera desired=(%.3f,%.3f)/%.4f actual=(%.3f,%.3f)/%.4f\n",savedView["center_x"].toDouble(),savedView["center_y"].toDouble(),savedView["scale"].toDouble(),loadedCenter.x(),loadedCenter.y(),view->getScale());require(std::abs(view->getScale()-savedView["scale"].toDouble())<.001 && QLineF(loadedCenter,QPointF(savedView["center_x"].toDouble(),savedView["center_y"].toDouble())).length()<5/view->getScale(),"Preset camera not restored");
        require(graph->delegateModel<AudioNodeModel>(input)->adapter().value("stream_id")=="stream-main","Preset loses network source metadata");
        const auto savedPosition=graph->nodeData(converter,QtNodes::NodeRole::Position).toPointF();editor->hide();editor->show();QCoreApplication::processEvents();require(graph->nodeData(converter,QtNodes::NodeRole::Position).toPointF()==savedPosition,"Preset explicit layout repeated on show");
        auto malformed=saved;malformed["id"]="invalid-presentation";malformed["presentation"]=QJsonObject{{"version",2}};app._ctx.file_configs.ui.pipeline_presets.append(malformed);const auto beforeInvalid=editor->document();require(!editor->load_preset("invalid-presentation") && editor->document()==beforeInvalid,"Future presentation mutates draft");
        domain::NumericProcessingGraph typed;QString exportError;const bool exported=editor->numeric_draft(typed,&exportError);
        if(!exported)std::fprintf(stderr,"Typed export diagnostic: %s\n",qPrintable(exportError));
        require(exported,"Typed export failed");QByteArray canonical;require(bool(desktop::encode_numeric_graph(typed,canonical)),"Canonical encoding failed");
        app._ctx.file_configs.ui.pipeline_presets.append(QJsonObject{{"version",1},{"id","no-layout"},{"name","No coordinates"},{"graph",QJsonDocument::fromJson(canonical).object()}});
        editor->hide();require(editor->load_preset("no-layout"),"Coordinate-free preset rejected");require(graph->nodeData(converter,QtNodes::NodeRole::Position).toPointF()!=savedPosition,"Coordinate-free preset inherited preceding positions");editor->show();require(wait_until([&]{return graph->nodeData(converter,QtNodes::NodeRole::Position).toPointF()!=QPointF{};}),"Deferred preset layout missing");
        graph->setNodeData(converter,QtNodes::NodeRole::Position,QPointF(300,400));editor->hide();editor->show();QCoreApplication::processEvents();require(graph->nodeData(converter,QtNodes::NodeRole::Position).toPointF()==QPointF(300,400),"Preset arrange repeats after user move");
        trigger(window,"example-stereo-sources");require(wait_until([&]{return graph->allNodeIds().size()>=10 && graph->nodeData(2,QtNodes::NodeRole::Position).toPointF()!=QPointF{};}),"Actual stereo example action did not arrange graph");
        const auto stereo=graph->numeric_graph();
        const auto selector=std::find_if(stereo.nodes.begin(),stereo.nodes.end(),[](const auto& node){return node.kind=="InputSelector";});
        require(selector!=stereo.nodes.end() && selector->outputs.size()==2 && selector->inputs.size()==6,"Stereo example lost its three grouped stereo inputs");
        std::size_t generators=0;for(const auto& node:stereo.nodes)if(node.kind=="SignalGenerator"){
            ++generators;require(node.outputs.size()==1 && domain::has_configuration_binding(stereo,node.id,domain::ConfigurationRole::SampleClock),"Stereo example generator lacks explicit mono stream or SampleClock binding");
        }
        require(generators==2,"Stereo example requires two independent mono generators");
        const auto defaultPosition=graph->nodeData(2,QtNodes::NodeRole::Position).toPointF();editor->hide();editor->show();QCoreApplication::processEvents();require(graph->nodeData(2,QtNodes::NodeRole::Position).toPointF()==defaultPosition,"Default layout repeated on show");
        require(wait_until([&]{return app.presenter().client().snapshot().graph_revision>initialRevision && !app.presenter().graph_request_pending();}) && !applied.isEmpty() && applied.last()[1].toBool() && app.presenter().client().snapshot().statuses.value("analyzer").state==pipeline::State::Running,"Default action did not apply continuous processing");
        std::puts("canonical editor actions: create/edit/copy/paste/undo/redo/delete/routes, exact presets, deferred layout/default and manual Apply isolation PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
