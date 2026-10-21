#include "isolated_gui_platform.h"
#include "synthetic_capture.h"
#include "editor_ui_test_fixture.h"
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
#include <QtTest/QSignalSpy>
#include <cstdio>
#include <stdexcept>
#include <algorithm>
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
using editor_ui_test_fixture::wait_until;
static void trigger(QWidget* root,const char* name){
    if(QString::fromLatin1(name)=="undoDataFlow" || QString::fromLatin1(name)=="redoDataFlow") {
        root->activateWindow(); auto* canvas=root->findChild<QtNodes::GraphicsView*>();
        require(canvas,"history editor context"); canvas->setFocus(); QCoreApplication::processEvents();
    }
    auto* action=root->findChild<QAction*>(name);require(action && action->isEnabled(),name);action->trigger();QCoreApplication::processEvents();
}
static QAction* shortcut(QtNodes::GraphicsView* view,QKeySequence::StandardKey key){for(const auto actions=view->actions();auto* action:actions)if(action->shortcuts().contains(QKeySequence(key)))return action;throw std::runtime_error("Canvas action missing");}
struct ActionContext {
    CoreApplication& app;
    MainWindow* window;
    SchemeEditorWidget* editor;
    NumericEditorGraphModel* graph;
    QtNodes::DataFlowGraphicsScene* scene;
    QtNodes::GraphicsView* view;
};
struct ActionNodes {QtNodes::NodeId input,route,output,converter;};
static QtNodes::NodeId create_node(ActionContext& context,const QString& kind){
    auto* library=context.editor->findChild<QTreeWidget*>("pipelineNodeLibrary");
    require(library,"Editor library missing");
    const auto before=context.graph->allNodeIds();
    QTreeWidgetItem* entry=nullptr;
    for(QTreeWidgetItemIterator item(library);*item;++item)
        if((*item)->data(0,Qt::UserRole).toString()==kind)entry=*item;
    require(entry,"Library kind missing");
    emit library->itemDoubleClicked(entry,0);QCoreApplication::processEvents();
    for(auto id:context.graph->allNodeIds())if(!before.contains(id))return id;
    throw std::runtime_error("Library action did not create node");
}
static ActionNodes create_action_nodes(ActionContext& context){
    return {create_node(context,"NetworkInput"),create_node(context,"Switch"),
            create_node(context,"NetworkOutput"),create_node(context,"Converter")};
}
static void exercise_network_adapter(ActionContext& context,QtNodes::NodeId inputId){
    auto* input=context.graph->delegateModel<AudioNodeModel>(inputId);
    std::unique_ptr<QWidget> form(input->parameter_widget());
    auto* stream=form->findChild<QLineEdit*>("adapterStreamId");
    require(stream,"Registered network source control missing");
    const auto dspBefore=context.graph->numeric_graph();
    stream->setText("stream-main");stream->setModified(true);
    QMetaObject::invokeMethod(stream,"editingFinished",Qt::DirectConnection);
    require(input->adapter().value("stream_id")=="stream-main","Actual source metadata edit missing");
    trigger(context.window,"undoDataFlow");require(input->adapter().isEmpty(),"Source metadata Undo failed");
    trigger(context.window,"redoDataFlow");require(stream->text()=="stream-main","Source metadata Redo does not sync form");
    QByteArray before,after;
    desktop::encode_numeric_graph(dspBefore,before);
    desktop::encode_numeric_graph(context.graph->numeric_graph(),after);
    require(before==after,"Application adapter changes DSP DTO");
}
static void exercise_typed_routes(ActionContext& context,ActionNodes nodes){
    context.graph->addConnection({nodes.input,0,nodes.route,0});
    context.graph->addConnection({nodes.route,0,nodes.output,0});
    require(context.graph->connectionExists({nodes.input,0,nodes.route,0}) &&
            context.graph->connectionExists({nodes.route,0,nodes.output,0}),"Typed routes not created");
}
static void exercise_exact_parameter(ActionContext& context,QtNodes::NodeId converter){
    auto* model=context.graph->delegateModel<AudioNodeModel>(converter);
    std::unique_ptr<QWidget> form(model->parameter_widget());
    auto* type=form->findChild<QComboBox*>("numericNan_type");
    auto* literal=form->findChild<QLineEdit*>("numericNan");
    require(type && literal,"Exact parameter controls missing");
    type->setCurrentIndex(1);literal->setText("18446744073709551615");literal->setModified(true);
    QMetaObject::invokeMethod(literal,"editingFinished",Qt::DirectConnection);
    auto nan=[&]{return std::get<domain::ConverterProcessingParameters>(model->numeric_node()->parameters).policy.nan_replacement;};
    require(std::get<std::uint64_t>(nan())==UINT64_MAX,"Actual edit rounds UInt64");
    trigger(context.window,"undoDataFlow");require(std::get<std::uint64_t>(nan())==0,"File Undo does not undo parameter edit");
    trigger(context.window,"redoDataFlow");require(std::get<std::uint64_t>(nan())==UINT64_MAX,"File Redo loses exact parameter");
    auto* visible=form->findChild<QAction*>("showOnBlock_numericNan");
    require(visible,"Presentation toggle missing");visible->trigger();
}
static QtNodes::NodeId copy_selected(ActionContext& context,QtNodes::NodeId id){
    context.scene->clearSelection();context.scene->nodeGraphicsObject(id)->setSelected(true);
    const auto before=context.graph->allNodeIds();
    shortcut(context.view,QKeySequence::Copy)->trigger();
    shortcut(context.view,QKeySequence::Paste)->trigger();QCoreApplication::processEvents();
    QtNodes::NodeId copied=QtNodes::InvalidNodeId;
    for(auto current:context.graph->allNodeIds())if(!before.contains(current))copied=current;
    require(copied!=QtNodes::InvalidNodeId && context.graph->allNodeIds().size()==before.size()+1,
            "Actual Copy/Paste failed");
    return copied;
}
static void exercise_converter_copy(ActionContext& context,QtNodes::NodeId converter){
    auto copied=copy_selected(context,converter);
    auto* source=context.graph->delegateModel<AudioNodeModel>(converter);
    auto* duplicate=context.graph->delegateModel<AudioNodeModel>(copied);
    require(domain::same_processing_parameters(source->numeric_node()->parameters,duplicate->numeric_node()->parameters) &&
            duplicate->parameter_on_block("numericNan"),"Copy/Paste loses exact parameter or block flag");
    trigger(context.window,"undoDataFlow");require(!context.graph->nodeExists(copied),"Paste Undo leaves node");
    trigger(context.window,"redoDataFlow");require(context.graph->nodeExists(copied),"Paste Redo loses node");
}
static void exercise_partial_route_copy(ActionContext& context,QtNodes::NodeId route){
    const auto copiedRoute=copy_selected(context,route);
    require(context.graph->connections(copiedRoute,QtNodes::PortType::In,0).empty() &&
            context.graph->connections(copiedRoute,QtNodes::PortType::Out,0).empty(),
            "Copy partial selection aliases external route IDs");
}
static void exercise_route_delete(ActionContext& context,ActionNodes nodes){
    context.scene->clearSelection();context.scene->nodeGraphicsObject(nodes.output)->setSelected(true);
    context.view->deleteSelectionAction()->trigger();
    require(!context.graph->nodeExists(nodes.output) &&
            !context.graph->connectionExists({nodes.route,0,nodes.output,0}),"Delete action leaves route");
    trigger(context.window,"undoDataFlow");
    require(context.graph->nodeExists(nodes.output) &&
            context.graph->connectionExists({nodes.route,0,nodes.output,0}),"Delete Undo loses route");
}
static QJsonObject save_layout_preset(ActionContext& context,QtNodes::NodeId converter){
    require(context.graph->nodeData(converter,QtNodes::NodeRole::Position).toPointF()==QPointF(1234,5678),
            "Preset fixture position changed before save");
    require(context.editor->save_preset("canonical exact layout"),"Preset save failed");
    return context.app._ctx.file_configs.ui.pipeline_presets.last().toObject();
}
static void verify_saved_camera(ActionContext& context,const QJsonObject& savedView){
    const auto loadedCenter=context.view->mapToScene(context.view->viewport()->rect().center());
    std::fprintf(stderr,"Preset camera desired=(%.3f,%.3f)/%.4f actual=(%.3f,%.3f)/%.4f\n",
        savedView["center_x"].toDouble(),savedView["center_y"].toDouble(),savedView["scale"].toDouble(),
        loadedCenter.x(),loadedCenter.y(),context.view->getScale());
    require(std::abs(context.view->getScale()-savedView["scale"].toDouble())<.001 &&
            QLineF(loadedCenter,QPointF(savedView["center_x"].toDouble(),savedView["center_y"].toDouble())).length()<
                5/context.view->getScale(),"Preset camera not restored");
}
static void reject_future_presentation(ActionContext& context,const QJsonObject& saved);
static void restore_saved_preset(ActionContext& context,ActionNodes nodes,
                                 const QJsonObject& saved,const QJsonObject& savedView){
    context.graph->setNodeData(nodes.converter,QtNodes::NodeRole::Position,QPointF(-9000,-9000));
    context.view->centerOn(0,0);
    require(context.editor->load_preset(saved["id"].toString()),"Preset load failed");
    QCoreApplication::processEvents();
    require(context.graph->nodeData(nodes.converter,QtNodes::NodeRole::Position).toPointF()==QPointF(1234,5678),
            "Preset inherits previous graph positions");
    verify_saved_camera(context,savedView);
    require(context.graph->delegateModel<AudioNodeModel>(nodes.input)->adapter().value("stream_id")=="stream-main",
            "Preset loses network source metadata");
    const auto savedPosition=context.graph->nodeData(nodes.converter,QtNodes::NodeRole::Position).toPointF();
    context.editor->hide();context.editor->show();QCoreApplication::processEvents();
    require(context.graph->nodeData(nodes.converter,QtNodes::NodeRole::Position).toPointF()==savedPosition,
            "Preset explicit layout repeated on show");
}
static void exercise_saved_preset(ActionContext& context,ActionNodes nodes){
    context.graph->setNodeData(nodes.converter,QtNodes::NodeRole::Position,QPointF(1234,5678));
    context.view->setupScale(.7);context.view->centerOn(1300,5700);
    context.editor->flush_presentation();
    const auto savedView=context.app._ctx.file_configs.ui.editor_view;
    const auto saved=save_layout_preset(context,nodes.converter);
    restore_saved_preset(context,nodes,saved,savedView);
    reject_future_presentation(context,saved);
}
static void reject_future_presentation(ActionContext& context,const QJsonObject& saved){
    auto malformed=saved;malformed["id"]="invalid-presentation";
    malformed["presentation"]=QJsonObject{{"version",2}};
    context.app._ctx.file_configs.ui.pipeline_presets.append(malformed);
    const auto beforeInvalid=context.editor->document();
    require(!context.editor->load_preset("invalid-presentation") &&
            context.editor->document()==beforeInvalid,"Future presentation mutates draft");
}
static void exercise_coordinate_free_preset(ActionContext& context,QtNodes::NodeId converter){
    domain::NumericProcessingGraph typed;QString exportError;
    const bool exported=context.editor->numeric_draft(typed,&exportError);
    if(!exported)std::fprintf(stderr,"Typed export diagnostic: %s\n",qPrintable(exportError));
    require(exported,"Typed export failed");
    QByteArray canonical;
    require(bool(desktop::encode_numeric_graph(typed,canonical)),"Canonical encoding failed");
    context.app._ctx.file_configs.ui.pipeline_presets.append(
        QJsonObject{{"version",1},{"id","no-layout"},{"name","No coordinates"},{"graph",QJsonDocument::fromJson(canonical).object()}});
    const auto prior=context.graph->nodeData(converter,QtNodes::NodeRole::Position).toPointF();
    context.editor->hide();
    require(context.editor->load_preset("no-layout"),"Coordinate-free preset rejected");
    require(context.graph->nodeData(converter,QtNodes::NodeRole::Position).toPointF()!=prior,
            "Coordinate-free preset inherited preceding positions");
    context.editor->show();
    require(wait_until([&]{return context.graph->nodeData(converter,QtNodes::NodeRole::Position).toPointF()!=QPointF{};}),
            "Deferred preset layout missing");
    context.graph->setNodeData(converter,QtNodes::NodeRole::Position,QPointF(300,400));
    context.editor->hide();context.editor->show();QCoreApplication::processEvents();
    require(context.graph->nodeData(converter,QtNodes::NodeRole::Position).toPointF()==QPointF(300,400),
            "Preset arrange repeats after user move");
}
static void verify_stereo_graph(const domain::NumericProcessingGraph& stereo){
    const auto selector=std::find_if(stereo.nodes.begin(),stereo.nodes.end(),
        [](const auto& node){return node.kind=="InputSelector";});
    require(selector!=stereo.nodes.end() && selector->outputs.size()==3 && selector->inputs.size()==9,
            "Stereo example must expose two mono outputs plus Fs and three groups of mono/Fs inputs");
    for(domain::PortId input=0;input<9;++input)
        require(std::count_if(stereo.edges.begin(),stereo.edges.end(),[&](const auto& edge){
            return edge.to==selector->id && edge.input_port==input;
        })==1,"Stereo example lost a routed selector input");
    std::size_t generators=0;
    for(const auto& node:stereo.nodes)if(node.kind=="SignalGenerator"){
        ++generators;require(node.outputs.size()==2 &&
            domain::has_configuration_binding(stereo,node.id,domain::ConfigurationRole::SampleClock),
            "Stereo example generator lacks mono data/Fs outputs or SampleClock binding");
    }
    require(generators==2,"Stereo example requires two independent mono generators");
}
static void verify_stereo_example(ActionContext& context,quint64 initialRevision,QSignalSpy& applied){
    trigger(context.window,"example-stereo-sources");
    require(wait_until([&]{return context.graph->allNodeIds().size()>=10 &&
            context.graph->nodeData(2,QtNodes::NodeRole::Position).toPointF()!=QPointF{};}),
            "Actual stereo example action did not arrange graph");
    verify_stereo_graph(context.graph->numeric_graph());
    const auto position=context.graph->nodeData(2,QtNodes::NodeRole::Position).toPointF();
    context.editor->hide();context.editor->show();QCoreApplication::processEvents();
    require(context.graph->nodeData(2,QtNodes::NodeRole::Position).toPointF()==position,
            "Default layout repeated on show");
    require(wait_until([&]{return context.app.presenter().client().snapshot().graph_revision>initialRevision &&
            !context.app.presenter().graph_request_pending();}) &&
            !applied.isEmpty() && applied.last()[1].toBool() &&
            context.app.presenter().client().snapshot().statuses.value("analyzer").state==pipeline::State::Running,
            "Default action did not apply continuous processing");
}
static void run_action_oracles(CoreApplication& app){
    auto* window=editor_ui_test_fixture::main_window(require);window->show();
    auto* editor=editor_ui_test_fixture::editor(*window,require);
    auto* graph=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
    auto* scene=editor_ui_test_fixture::scene(*editor,require);
    auto* view=editor_ui_test_fixture::view(*editor,require);
    require(graph && editor->findChild<QTreeWidget*>("pipelineNodeLibrary"),"Editor components missing");
    ActionContext context{app,window,editor,graph,scene,view};
    require(wait_until([&]{return editor->isVisible() && app.presenter().client().snapshot().graph_revision>0 &&
            !app.presenter().graph_request_pending();}),"Initial graph not applied");
    const auto initialRevision=app.presenter().client().snapshot().graph_revision;
    QSignalSpy applied(&app.presenter(),&GuiPresenter::graph_apply_finished);
    const auto nodes=create_action_nodes(context);
    exercise_network_adapter(context,nodes.input);
    exercise_typed_routes(context,nodes);
    exercise_exact_parameter(context,nodes.converter);
    exercise_converter_copy(context,nodes.converter);
    exercise_partial_route_copy(context,nodes.route);
    exercise_route_delete(context,nodes);
    exercise_saved_preset(context,nodes);
    exercise_coordinate_free_preset(context,nodes.converter);
    verify_stereo_example(context,initialRevision,applied);
}
int main(int argc,char** argv){
    if(!test_fixture::isolated_gui_platform())return 2;
    try{
        QTemporaryDir temporary;require(temporary.isValid(),"Temporary directory missing");
        const auto path=temporary.filePath("project.json");
        domain::NumericProcessingGraph empty;empty.graph_id="main";
        desktop::NumericDraftPresentation presentation;
        presentation.application_settings={{"ui",QJsonObject{{"autosave_enabled",false},{"pipeline_auto_apply",false},
            {"language","en"},{"show_spectrogram",false},{"show_data_flow",true}}}};
        require(editor_ui_test_fixture::save_project(path,empty,presentation,require),
                "Saved canonical fixture creation failed");
        auto encoded=path.toLocal8Bit();
        char executable[]="numeric_editor_action_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory(false));
        run_action_oracles(app);
        std::puts("canonical editor actions: create/edit/copy/paste/undo/redo/delete/routes, exact presets, deferred layout/default and manual Apply isolation PASS");
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
