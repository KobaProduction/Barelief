#include "synthetic_capture.h"
#include "editor_ui_test_fixture.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/node_parameter_form.h>
#include <core_app/ui/collapsible_section.h>
#include <desktop/numeric_project_document.h>
#include <domain/graph/preparation_values.h>
#include <visualization/ui_text.h>
#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QThread>
#include <QProcess>
#include <QFile>
#include <QJsonArray>
#include <QtTest/QSignalSpy>
#include <cmath>
#include <cstdio>
#include <functional>
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

static void require(bool value,const char* text) { if(!value)throw std::runtime_error(text); }
static bool wait_for(const std::function<bool()>& predicate) {
    return editor_ui_test_fixture::wait_until(predicate,3000);
}

class Gestures {
public:
    SchemeEditorWidget& editor;
    NumericEditorGraphModel& model;
    QtNodes::GraphicsView* view;
    QtNodes::DataFlowGraphicsScene* scene;
    Gestures(SchemeEditorWidget& e,NumericEditorGraphModel& m):editor(e),model(m),
        view(editor_ui_test_fixture::view(e,require)),scene(editor_ui_test_fixture::scene(e,require)) {
    }
    void send(QEvent::Type type,QPoint point,Qt::MouseButton button,Qt::MouseButtons buttons,
              Qt::KeyboardModifiers modifiers=Qt::NoModifier){
        QMouseEvent event(type,point,view->viewport()->mapToGlobal(point),button,buttons,modifiers);
        QApplication::sendEvent(view->viewport(),&event);
    }
    QTreeWidgetItem* library_entry(const QString& kind) const {
        auto* library=editor.findChild<QTreeWidget*>("pipelineNodeLibrary");
        require(library,"Actual editor library absent");
        for(QTreeWidgetItemIterator item(library);*item;++item)
            if((*item)->data(0,Qt::UserRole).toString()==kind)return *item;
        throw std::runtime_error("Requested registered library item absent");
    }
    QtNodes::NodeId create(const QString& kind) {
        auto* library=editor.findChild<QTreeWidget*>("pipelineNodeLibrary");
        const auto before=model.allNodeIds();
        emit library->itemDoubleClicked(library_entry(kind),0);
        QCoreApplication::processEvents();
        for(auto id:model.allNodeIds())if(!before.contains(id))return id;
        throw std::runtime_error("Actual library action did not create node");
    }
    QList<QGraphicsItem*> hide_other_nodes(QtNodes::NodeId first,QtNodes::NodeId second){
        QList<QGraphicsItem*> hidden;
        for(auto* item:scene->items())if(!item->parentItem() && item->isVisible()) {
            auto* node=qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item);
            if(!node || (node->nodeId()!=first && node->nodeId()!=second)){hidden.append(item);item->hide();}
        }
        return hidden;
    }
    QPoint port_position(QtNodes::NodeId id,QtNodes::PortType side,QtNodes::PortIndex port) const {
        auto* node=scene->nodeGraphicsObject(id);
        return view->mapFromScene(scene->nodeGeometry().portScenePosition(id,side,port,node->sceneTransform()));
    }
    void drag_between_ports(QtNodes::NodeId from,QtNodes::PortIndex output,QPoint start,QPoint end){
        send(QEvent::MouseMove,start,Qt::NoButton,Qt::NoButton);
        send(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        require(scene->mouseGrabberItem() && scene->mouseGrabberItem()->type()!=QtNodes::NodeGraphicsObject::Type,
                "Port press did not create QtNodes draft connection");
        const auto actual=static_cast<QtNodes::ConnectionGraphicsObject*>(scene->mouseGrabberItem())->connectionId();
        require(actual.outNodeId==from && actual.outPortIndex==output,
                "Port geometry selected a different actual output identity");
        for(int step=1;step<=6;++step)send(QEvent::MouseMove,start+(end-start)*step/6,Qt::NoButton,Qt::LeftButton);
        send(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
        QCoreApplication::processEvents();
        require(!scene->mouseGrabberItem(),"Port release retained draft mouse grabber");
    }
    void drag(QtNodes::NodeId from,QtNodes::PortIndex output,QtNodes::NodeId to,QtNodes::PortIndex input) {
        // Only this fixture's offscreen Qt widgets receive events. No OS input,
        // cursor APIs, window activation or screenshots are involved.
        model.setNodeData(from,QtNodes::NodeRole::Position,QPointF(0,0));
        const double targetX=std::max(650.0,scene->nodeGraphicsObject(from)->boundingRect().width()+250.0);
        model.setNodeData(to,QtNodes::NodeRole::Position,QPointF(targetX,0));
        auto hidden=hide_other_nodes(from,to);
        view->setupScale(std::min(.65,view->viewport()->width()/(targetX+450.0)));
        view->setSceneRect(QRectF(-500,-500,3000,2000));view->centerOn(targetX/2+180,120);
        QCoreApplication::processEvents();
        const auto start=port_position(from,QtNodes::PortType::Out,output);
        const auto end=port_position(to,QtNodes::PortType::In,input);
        if(!view->viewport()->rect().contains(start) || !view->viewport()->rect().contains(end))std::fprintf(stderr,"own viewport=%dx%d start=(%d,%d) end=(%d,%d) visible=%d\n",view->viewport()->width(),view->viewport()->height(),start.x(),start.y(),end.x(),end.y(),view->isVisible());
        require(view->viewport()->rect().contains(start) && view->viewport()->rect().contains(end),"Gesture ports outside own viewport");
        drag_between_ports(from,output,start,end);
        for(auto* item:hidden)item->show();
    }
    void reorder(QtNodes::NodeId id,QtNodes::PortType side,QtNodes::PortIndex from,QtNodes::PortIndex to){
        auto* node=scene->nodeGraphicsObject(id);require(node,"Reorder fixture node absent");
        QList<QGraphicsItem*> hidden;for(auto* item:scene->items())if(!item->parentItem() && item!=node && item->isVisible()){hidden.append(item);item->hide();}
        scene->nodeGeometry().recomputeSize(id);view->setupScale(.8);view->centerOn(node);
        QCoreApplication::processEvents();
        const auto start=port_position(id,side,from),end=port_position(id,side,to);
        const auto edges=model.allConnectionIds(id);auto* delegate=model.delegateModel<AudioNodeModel>(id);const auto parameters=delegate->numeric_node()->parameters;const int undo=editor.history().index();
        send(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton,Qt::AltModifier);
        send(QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton,Qt::AltModifier);
        send(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton,Qt::AltModifier);QCoreApplication::processEvents();
        verify_reorder(id,side,from,to,start,end,edges,parameters,undo,*delegate);
        for(auto* item:hidden)item->show();
    }
    void verify_reorder(QtNodes::NodeId id,QtNodes::PortType side,QtNodes::PortIndex from,QtNodes::PortIndex to,
                        QPoint start,QPoint end,const std::unordered_set<QtNodes::ConnectionId>& edges,
                        const domain::ProcessingParameters& parameters,int undo,AudioNodeModel& delegate){
        const auto order=delegate.visual_port_order(side);
        if(order.front()!=from || model.allConnectionIds(id)!=edges || !domain::same_processing_parameters(parameters,delegate.numeric_node()->parameters))std::fprintf(stderr,"Alt reorder node=%u start=%d,%d end=%d,%d first=%u from=%u edges=%d parameters=%d undo=%d/%d\n",unsigned(id),start.x(),start.y(),end.x(),end.y(),unsigned(order.front()),unsigned(from),int(model.allConnectionIds(id)==edges),int(domain::same_processing_parameters(parameters,delegate.numeric_node()->parameters)),undo,editor.history().index());
        require(order.front()==from && model.allConnectionIds(id)==edges && domain::same_processing_parameters(parameters,delegate.numeric_node()->parameters),"Alt port reorder changed stable wiring or DSP parameters");
        require(editor.history().index()==undo+1,"Alt reorder must be a presentation undo command");editor.history().undo();require(delegate.visual_port_order(side).front()==to && model.allConnectionIds(id)==edges,"Alt reorder undo changed wiring");editor.history().redo();require(delegate.visual_port_order(side).front()==from,"Alt reorder redo lost visual order");
    }
};

static int run_layout_reload(char** argv){
    require(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen","Layout probe requires offscreen");
    auto encoded=QByteArray(argv[2]);
    char name[]="numeric_editor_gesture_tests",option[]="--config";
    char* arguments[]={name,option,encoded.data(),nullptr};int count=3;
    CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
    auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
    const auto id=QString::fromLocal8Bit(argv[3]).toUInt();
    auto* node=model?model->delegateModel<AudioNodeModel>(id):nullptr;
    require(node,"Restored routing node missing");
    auto* window=editor_ui_test_fixture::main_window(require);
    auto* editor=editor_ui_test_fixture::editor(*window,require);
    const auto verify=[&]{return model->nodeData(id,QtNodes::NodeRole::Position).toPointF()==QPointF(777,555) &&
        node->visual_port_order(QtNodes::PortType::Out).front()==1 &&
        node->presentation().value("port_labels").toObject().value("output1")=="Saved output";};
    require(verify(),"Saved position/order/label was not restored");
    window->show();editor->show();
    QElapsedTimer settling;settling.start();
    while(settling.elapsed()<350){QCoreApplication::processEvents();QThread::msleep(2);}
    require(verify(),"First editor Show overwrote restored layout");
    require(app._ctx.file_configs.ui.editor_view.value("node_width").toInt()==280,
            "Saved node width was not restored");
    std::puts("disk restart + first Show preserves position, Alt port order, label and node width PASS");
    return 0;
}
struct GestureScenario {
    CoreApplication& app;
    GuiPresenter& presenter;
    NumericEditorGraphModel& model;
    MainWindow& window;
    SchemeEditorWidget& editor;
    Gestures& gestures;
};
static QtNodes::NodeId exercise_basic_gestures(GestureScenario& scenario){
    auto& model=scenario.model;auto& editor=scenario.editor;auto& gestures=scenario.gestures;
    const auto network=gestures.create("NetworkInput"),networkSink=gestures.create("NetworkOutput");
    const int beforeConnect=editor.history().count();gestures.drag(network,0,networkSink,0);
    require(model.connectionExists({network,0,networkSink,0}) && editor.history().count()==beforeConnect+1,
            "Compatible actual port drag failed");
    std::puts("actual QtNodes compatible press/move/release PASS");
    const auto route=gestures.create("Switch"),sink=gestures.create("NetworkOutput");
    gestures.drag(network,0,route,0);gestures.drag(route,1,sink,0);
    gestures.reorder(route,QtNodes::PortType::Out,1,0);
    std::puts("Alt reorder with connected stable port IDs and Undo/Redo PASS");
    return route;
}
static void verify_disk_layout(GestureScenario& scenario,QTemporaryDir& temporary,QtNodes::NodeId route){
    auto* routeModel=scenario.model.delegateModel<AudioNodeModel>(route);
    auto display=routeModel->presentation();
    display["port_labels"]=QJsonObject{{"output1","Saved output"}};
    require(scenario.model.edit_presentation(route,display),"Label setup");
    scenario.gestures.scene->nodeGraphicsObject(route)->setPos(777,555);
    scenario.app._ctx.file_configs.ui.editor_view["node_width"]=280;
    const auto layoutPath=temporary.filePath("saved-layout.json");
    scenario.app.save_configuration_as(layoutPath);
    require(QFile::exists(layoutPath),"Actual application did not save layout");
    emit scenario.window.on_config_updated();
    QProcess restart;
    restart.start(QCoreApplication::applicationFilePath(),
                  {"--layout-reload",layoutPath,QString::number(route)});
    require(restart.waitForFinished(8000),"Layout restart timed out");
    if(restart.exitCode()!=0)std::fprintf(stderr,"layout restart: %s\n",restart.readAllStandardError().constData());
    require(restart.exitStatus()==QProcess::NormalExit && restart.exitCode()==0,
            "Layout did not survive application restart");
}
static domain::ConfiguredProcessingNode invalid_converter(const signal_contracts::NumericContract& scalar){
    auto invalidConverter=numeric_editor::default_node("Converter");invalidConverter.id=14;
    require(numeric_editor::set_contract(invalidConverter,domain::PortDirection::Input,0,scalar),
            "Reported converter input fixture invalid");
    auto mel=scalar;mel.dtype=signal_contracts::DType::Float32;
    mel.semantic=signal_contracts::Semantic::MelEnergy;
    mel.shape={signal_contracts::Rank::Vector,{signal_contracts::Axis::Band,signal_contracts::Axis::None},
               {signal_contracts::ExtentBounds{50,50},signal_contracts::ExtentBounds{}}};
    require(numeric_editor::set_contract(invalidConverter,domain::PortDirection::Output,0,mel),
            "Reported converter output fixture invalid");
    return invalidConverter;
}
static std::pair<domain::NumericProcessingGraph,domain::ConfiguredProcessingNode> matching_invalid_draft(){
    domain::NumericProcessingGraph screenshot;screenshot.graph_id="main";
    auto invalidSource=numeric_editor::default_node("SignalGenerator");invalidSource.id=1;
    auto scalar=std::get<signal_contracts::NumericContract>(invalidSource.outputs[0].contract.view());
    scalar.clock.sample_rate=44100;
    scalar.dtype=signal_contracts::DType::Int64;scalar.semantic=signal_contracts::Semantic::Scalar;
    scalar.shape={};scalar.coordinates={};scalar.channel_order=scalar.channel_order.first(1);
    require(numeric_editor::set_contract(invalidSource,domain::PortDirection::Output,0,scalar),
            "Reported source contract fixture invalid");
    auto invalidConverter=invalid_converter(scalar);
    screenshot.nodes={invalidSource,invalidConverter};
    return {std::move(screenshot),std::move(invalidSource)};
}
static void verify_invalid_contract_and_atomic_import(GestureScenario& scenario){
    const auto [screenshot,invalidSource]=matching_invalid_draft();
    require(scenario.editor.load_numeric_draft(screenshot),
            "Structurally valid draft must retain invalid contracts for diagnostics");
    domain::ResolvedProcessingNode resolved;
    require(!domain::resolve_processing_node(invalidSource,numeric_editor::limits(),resolved),
            "Matching labels must not bypass invalid configured source clock");
    auto malformed=screenshot;
    malformed.nodes.push_back(invalidSource);
    const auto before=scenario.editor.document();
    const auto undo=scenario.editor.history().index();
    require(!scenario.editor.load_numeric_draft(malformed) && scenario.editor.document()==before &&
            scenario.editor.history().index()==undo,
            "Duplicate-ID import must reject atomically without losing the current draft");
}
static QtNodes::NodeId connect_generator_to_window(GestureScenario& scenario){
    auto& model=scenario.model;auto& editor=scenario.editor;auto& gestures=scenario.gestures;
    require(editor.load_numeric_draft(numeric_default_graph()),"Shipped graph fixture refused");
    model.deleteNode(1);
    const auto signal=gestures.create("SignalGenerator"),clock=gestures.create("SampleClock");
    auto bound=*model.delegateModel<AudioNodeModel>(signal)->numeric_node();
    bound.clock_node=domain::NodeId(clock);
    require(model.edit_node(signal,bound),"Explicit generator clock binding failed");
    auto* generator=model.delegateModel<AudioNodeModel>(signal);
    auto* frequencyStep=model.delegateModel<AudioNodeModel>(1003);
    require(frequencyStep,"Shipped FrequencyStep missing from Apply fixture");
    const auto rate=generator->port_index(QtNodes::PortType::Out,domain::sample_rate_output);
    const auto stepRate=frequencyStep->port_index(QtNodes::PortType::In,0);
    require(rate!=QtNodes::InvalidPortIndex && stepRate!=QtNodes::InvalidPortIndex,
            "Explicit Fs ports missing from Apply fixture");
    const QtNodes::ConnectionId clockEdge{signal,rate,1003,stepRate};
    model.addConnection(clockEdge);
    require(model.connectionExists(clockEdge),"Fresh generator must restore FrequencyStep Fs input");
    gestures.drag(signal,0,2,0);
    const auto* source=model.delegateModel<AudioNodeModel>(signal)->numeric_node();
    const auto sourceContract=std::get<signal_contracts::NumericContract>(source->outputs[0].contract.view());
    std::fprintf(stderr,"fresh SignalGenerator channels=%zu; shipped Window channels=%zu; actual edge=%d\n",
        sourceContract.channel_order.size(),
        std::get<signal_contracts::NumericContract>(model.delegateModel<AudioNodeModel>(2)->numeric_node()->inputs[0].contract.view()).channel_order.size(),
        model.connectionExists({signal,0,2,0}));
    require(model.connectionExists({signal,0,2,0}),"Fresh generator cannot drag-connect shipped mono Window");
    gestures.drag(signal,0,9,0);
    require(model.connectionExists({signal,0,9,0}),"Fresh generator cannot drag-connect shipped AudioView");
    return signal;
}
static void connect_generator_to_views(GestureScenario& scenario,QtNodes::NodeId signal){
    auto& model=scenario.model;auto& gestures=scenario.gestures;
    const auto route=gestures.create("Switch");gestures.drag(signal,0,route,0);
    require(model.connectionExists({signal,0,route,0}),"Fresh generator cannot drag-connect fresh route");
    const auto sink=gestures.create("AudioView");gestures.drag(route,1,sink,0);
    require(model.connectionExists({route,1,sink,0}),"Actual drag lost stable second output identity");
    const auto mel=gestures.create("MelView");gestures.drag(signal,0,mel,0);
    require(model.connectionExists({signal,0,mel,0}),
            "Draft must retain an incompatible generator-to-Mel-view edge");
    scenario.editor.history().undo();
    require(!model.connectionExists({signal,0,mel,0}),
            "Undo must remove the incompatible Mel-view draft edge before Apply");
    model.deleteNode(mel);
    require(!model.nodeExists(mel),"Temporary Mel view must be removed before Apply");
}
static void exercise_incompatible_draft(GestureScenario& scenario,QtNodes::NodeId signal){
    auto& model=scenario.model;auto& editor=scenario.editor;auto& gestures=scenario.gestures;
    const auto incompatible=gestures.create("WLEDOutput");
    const auto validEdges=model.allConnectionIds(signal);
    const int beforeReject=editor.history().count();
    QSignalSpy rejected(&editor,&SchemeEditorWidget::status_message);
    gestures.drag(signal,0,incompatible,0);
    const QtNodes::ConnectionId edge{signal,0,incompatible,0};
    require(model.connectionExists(edge) && model.allConnectionIds(signal).size()==validEdges.size()+1 &&
            editor.history().count()>beforeReject,"Incompatible drag must remain in draft and Undo history");
    require(model.connection_visual_state(edge)==3 && !model.connection_failure(edge).isEmpty(),
            "Incompatible draft edge must expose a visible reason");
    editor.history().undo();require(!model.connectionExists(edge),"Undo removes the incompatible draft edge");
    editor.history().redo();require(model.connectionExists(edge),"Redo restores the incompatible draft edge");
    editor.history().undo();
}
static void refuse_invalid_source_field(GestureScenario& scenario,AudioNodeModel& source,QWidget& form,
                                        QtNodes::NodeId signal,const QJsonObject& before,
                                        const std::unordered_set<QtNodes::ConnectionId>& edges,int undo,const char* field){
    auto* choice=form.findChild<QComboBox*>(field);
    require(choice,"Registered source contract editor missing");
    require(choice->currentIndex()!=0,"Unsupported source choice fixture is already selected");
    QSignalSpy refusal(&scenario.editor,&SchemeEditorWidget::status_message);
    choice->setCurrentIndex(0);QCoreApplication::processEvents();
    require(source.save()==before && scenario.model.allConnectionIds(signal)==edges &&
            scenario.editor.history().index()==undo,"Unsupported source choice changed node/edges/undo");
    const auto* row=form.findChild<QWidget*>(QString::fromLatin1(field)+"_row");
    auto* inlineError=row?row->findChild<QLabel*>("validationMessage"):nullptr;
    const bool fieldRefusal=inlineError&&inlineError->isVisible()&&!inlineError->text().isEmpty()&&
        choice->property("validationError").toBool()&&choice->accessibleDescription()==inlineError->text();
    const bool editorRefusal=!refusal.isEmpty()&&!refusal.last()[0].toString().isEmpty();
    if(!fieldRefusal&&!editorRefusal)std::fprintf(stderr,
        "unsupported source field=%s restoredIndex=%d inline=%s visible=%d statusEvents=%lld\n",
        field,choice->currentIndex(),inlineError?qPrintable(inlineError->text()):"<missing>",
        inlineError&&inlineError->isVisible(),static_cast<long long>(refusal.size()));
    require(fieldRefusal||editorRefusal,"Unsupported source choice has no visible refusal");
}
static void exercise_invalid_source_edits(GestureScenario& scenario,QtNodes::NodeId signal){
    auto* source=scenario.model.delegateModel<AudioNodeModel>(signal);
    std::unique_ptr<QWidget> form(new NodeParameterForm(source->parameter_model(),false,nullptr,true));
    form->setAttribute(Qt::WA_ShowWithoutActivating);
    form->setWindowFlag(Qt::WindowDoesNotAcceptFocus,true);
    for(auto* section:form->findChildren<ui::CollapsibleSection*>())section->setExpanded(true);
    form->show();QCoreApplication::processEvents();
    const auto before=source->save();
    const auto edges=scenario.model.allConnectionIds(signal);
    const int undo=scenario.editor.history().index();
    for(const auto field:{"output0Dtype","output0Rank"})
        refuse_invalid_source_field(scenario,*source,*form,signal,before,edges,undo,field);
    form->hide();
    auto impossible=*source->numeric_node();
    require(numeric_editor::set_dtype(impossible,domain::PortDirection::Output,0,signal_contracts::DType::Int64),
            "Direct source rejection fixture contract is not valid");
    domain::ResolvedProcessingNode resolved;
    require(!domain::resolve_processing_node(impossible,numeric_editor::limits(),resolved),
            "Generator must reject unsupported primitive at its operation boundary");
    require(!scenario.model.edit_node(signal,impossible) && source->save()==before &&
            scenario.model.allConnectionIds(signal)==edges,"Direct inadmissible source edit bypasses GUI guard");
}
struct DeliveryOracle {int delivered=0;std::uint64_t first=0,last=0;float peak=0;bool nonzero=false,progressing=false;};
static void observe_pcm(DeliveryOracle& result,const pipeline::DeliveredNumericFrame& frame){
    if(!frame.context || frame.context->endpoint.node!=9 || !frame.frame ||
       frame.status!=signal_contracts::Status::ok || !frame.frame.view().stamp.available)return;
    const auto data=frame.frame.view();
    const auto* numeric=std::get_if<signal_contracts::NumericView>(&data.payload);if(!numeric)return;
    for(std::size_t i=0;i<numeric->actual[0];++i){
        float value=0;
        require(signal_contracts::load_element(*numeric,{0,i,0},value)==signal_contracts::Status::ok &&
                std::isfinite(value),"Continuous source delivered invalid PCM");
        result.peak=std::max(result.peak,std::abs(value));
    }
    result.nonzero|=result.peak>.0001f;
    const auto sequence=data.stamp.sequence;
    if(!result.delivered)result.first=sequence;else result.progressing|=sequence>result.last;
    result.last=sequence;++result.delivered;
}
static void report_export_failure(const GestureScenario& scenario,const QString& error){
    domain::NumericProcessingGraph raw,evaluated,executable;
    if(!scenario.editor.numeric_draft(raw,nullptr,false)){
        std::fprintf(stderr,"Actual Apply raw draft unavailable: %s\n",qPrintable(error));return;
    }
    const auto diagnostic=domain::resolve_numeric_parameter_bindings(
        raw,numeric_editor::limits(),evaluated,executable);
    const auto node=std::find_if(raw.nodes.begin(),raw.nodes.end(),[&](const auto& value){
        return value.id==diagnostic.node;
    });
    std::fprintf(stderr,"Actual Apply export diagnostic: %s; resolver error=%d node=%lld kind=%s port=%u edge=%lld parameter=%.*s\n",
        qPrintable(error),int(diagnostic.error),static_cast<long long>(diagnostic.node),
        node==raw.nodes.end()?"<none>":node->kind.c_str(),unsigned(diagnostic.port),
        diagnostic.edge?static_cast<long long>(*diagnostic.edge):-1LL,
        int(diagnostic.parameter.size()),diagnostic.parameter.data());
}
static void apply_valid_draft(GestureScenario& scenario){
    QSignalSpy applied(&scenario.presenter,&GuiPresenter::graph_apply_finished);
    domain::NumericProcessingGraph graph;QString error;
    const bool exportable=scenario.editor.numeric_draft(graph,&error);
    if(!exportable)report_export_failure(scenario,error);
    require(exportable,"Actual Apply draft is not exportable");
    auto* apply=scenario.window.findChild<QPushButton*>("applyPipeline");
    require(apply,"Actual Apply control missing");apply->click();
    const bool replied=wait_for([&]{return !applied.isEmpty();});
    if(!replied || !applied.last()[1].toBool())std::fprintf(stderr,
        "Actual Apply diagnostic: reply=%d error=%s status=%s\n",replied,
        replied?qPrintable(applied.last()[2].toString()):"<no reply>",qPrintable(scenario.presenter.apply_status()));
    require(replied && applied.last()[1].toBool(),"Actual Apply rejects newly created connected generator");
    require(!scenario.window.findChild<QPushButton*>("pipelineRun") &&
            !scenario.window.findChild<QPushButton*>("pipelineStop"),"Obsolete Run/Stop controls returned");
}
static void observe_continuous_source(GestureScenario& scenario){
    DeliveryOracle delivery;
    const auto connection=QObject::connect(&scenario.presenter,&GuiPresenter::numeric_frame_ready,&scenario.editor,
                     [&](const pipeline::DeliveredNumericFrame& frame){observe_pcm(delivery,frame);});
    auto* raw=scenario.window.workspaceDock("rawSamplesDock");
    require(raw,"Continuous source graph dock missing");raw->toggleView(true);raw->setAsCurrentTab();
    const bool continuous=wait_for([&]{return delivery.delivered>=3 && delivery.nonzero && delivery.progressing;});
    if(!continuous)std::fprintf(stderr,
        "Continuous source diagnostic: delivered=%d peak=%.9g firstSequence=%llu lastSequence=%llu exposed=%d closed=%d\n",
        delivery.delivered,double(delivery.peak),(unsigned long long)delivery.first,
        (unsigned long long)delivery.last,raw->property("renderExposed").toBool(),raw->isClosed());
    QObject::disconnect(connection);
    require(continuous,"Continuous Apply has no progressing nonzero PCM sink frames");
}
static void run_gesture_scenario(CoreApplication& app,QTemporaryDir& temporary){
    auto& presenter=app.presenter();
    auto* model=dynamic_cast<NumericEditorGraphModel*>(presenter.state().data_flow_graph.get());
    auto* window=editor_ui_test_fixture::main_window(require);
    auto* editor=editor_ui_test_fixture::editor(*window,require);
    require(model,"Actual application editor model absent");
    window->resize(1600,900);window->show();editor->show();
    require(wait_for([&]{auto* view=editor->findChild<QtNodes::GraphicsView*>();
        return editor->isVisible() && view && view->viewport()->width()>300 &&
            view->viewport()->height()>150;}),"Own offscreen editor viewport did not become ready");
    Gestures gestures(*editor,*model);
    GestureScenario scenario{app,presenter,*model,*window,*editor,gestures};
    const auto route=exercise_basic_gestures(scenario);
    verify_disk_layout(scenario,temporary,route);
    verify_invalid_contract_and_atomic_import(scenario);
    const auto signal=connect_generator_to_window(scenario);
    connect_generator_to_views(scenario,signal);
    exercise_incompatible_draft(scenario,signal);
    exercise_invalid_source_edits(scenario,signal);
    apply_valid_draft(scenario);
    observe_continuous_source(scenario);
}
int main(int argc,char** argv) {
    if(qEnvironmentVariable("QT_QPA_PLATFORM")!="offscreen")return 2;
#ifdef Q_OS_WIN
    qputenv("QT_QPA_FONTDIR",(qEnvironmentVariable("WINDIR","C:/Windows")+"/Fonts").toUtf8());
#endif
    try {
        if(argc==4 && QByteArray(argv[1])=="--layout-reload")return run_layout_reload(argv);
        QTemporaryDir temporary;require(temporary.isValid(),"Fixture directory absent");
        const auto path=temporary.filePath("gesture-project.json");
        domain::NumericProcessingGraph empty;empty.graph_id="main";
        desktop::NumericDraftPresentation presentation;
        presentation.application_settings={{"ui",QJsonObject{{"autosave_enabled",false},{"pipeline_auto_apply",false},
            {"language","en"},{"show_data_flow",true}}}};
        require(editor_ui_test_fixture::save_project(path,empty,presentation,require),
                "Isolated canonical fixture refused");
        auto encoded=path.toLocal8Bit();
        char name[]="numeric_editor_gesture_tests",option[]="--config";
        char* arguments[]={name,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory(true));
        run_gesture_scenario(app,temporary);
        std::puts("fresh mono generator/Window/route actual gestures, rejected edits/diagnostics, Apply and continuous synthetic execution PASS");
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
