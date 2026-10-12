#include "synthetic_capture.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/ui/collapsible_section.h>
#include <desktop/numeric_project_document.h>
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
#include <QtTest/QSignalSpy>
#include <cmath>
#include <cstdio>
#include <functional>
#include <algorithm>
#include <stdexcept>

static void require(bool value,const char* text) { if(!value)throw std::runtime_error(text); }
static bool wait_for(const std::function<bool()>& predicate) {
    QElapsedTimer elapsed;elapsed.start();
    while(elapsed.elapsed()<3000) {
        QCoreApplication::processEvents();
        if(predicate())return true;
        QThread::msleep(2);
    }
    return predicate();
}

class Gestures {
public:
    SchemeEditorWidget& editor;
    NumericEditorGraphModel& model;
    QtNodes::GraphicsView* view;
    QtNodes::DataFlowGraphicsScene* scene;
    Gestures(SchemeEditorWidget& e,NumericEditorGraphModel& m):editor(e),model(m),
        view(e.findChild<QtNodes::GraphicsView*>()),scene(e.findChild<QtNodes::DataFlowGraphicsScene*>()) {
        require(view && scene,"Actual editor view/scene absent");
    }
    QtNodes::NodeId create(const QString& kind) {
        auto* library=editor.findChild<QTreeWidget*>("pipelineNodeLibrary");
        require(library,"Actual editor library absent");
        QTreeWidgetItem* entry=nullptr;
        for(QTreeWidgetItemIterator item(library);*item;++item)
            if((*item)->data(0,Qt::UserRole).toString()==kind)entry=*item;
        require(entry,"Requested registered library item absent");
        const auto before=model.allNodeIds();
        emit library->itemDoubleClicked(entry,0);
        QCoreApplication::processEvents();
        for(auto id:model.allNodeIds())if(!before.contains(id))return id;
        throw std::runtime_error("Actual library action did not create node");
    }
    void drag(QtNodes::NodeId from,QtNodes::PortIndex output,QtNodes::NodeId to,QtNodes::PortIndex input) {
        // Only this fixture's offscreen Qt widgets receive events. No OS input,
        // cursor APIs, window activation or screenshots are involved.
        model.setNodeData(from,QtNodes::NodeRole::Position,QPointF(0,0));
        const double targetX=std::max(650.0,scene->nodeGraphicsObject(from)->boundingRect().width()+250.0);
        model.setNodeData(to,QtNodes::NodeRole::Position,QPointF(targetX,0));
        QList<QGraphicsItem*> hidden;
        for(auto* item:scene->items())if(!item->parentItem() && item->isVisible()) {
            auto* node=qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item);
            if(!node || (node->nodeId()!=from && node->nodeId()!=to)){hidden.append(item);item->hide();}
        }
        view->setupScale(std::min(.65,view->viewport()->width()/(targetX+450.0)));
        view->setSceneRect(QRectF(-500,-500,3000,2000));view->centerOn(targetX/2+180,120);
        QCoreApplication::processEvents();
        const auto start=view->mapFromScene(scene->nodeGeometry().portScenePosition(from,QtNodes::PortType::Out,output,scene->nodeGraphicsObject(from)->sceneTransform()));
        const auto end=view->mapFromScene(scene->nodeGeometry().portScenePosition(to,QtNodes::PortType::In,input,scene->nodeGraphicsObject(to)->sceneTransform()));
        if(!view->viewport()->rect().contains(start) || !view->viewport()->rect().contains(end))std::fprintf(stderr,"own viewport=%dx%d start=(%d,%d) end=(%d,%d) visible=%d\n",view->viewport()->width(),view->viewport()->height(),start.x(),start.y(),end.x(),end.y(),view->isVisible());
        require(view->viewport()->rect().contains(start) && view->viewport()->rect().contains(end),"Gesture ports outside own viewport");
        auto send=[&](QEvent::Type type,QPoint point,Qt::MouseButton button,Qt::MouseButtons buttons) {
            QMouseEvent event(type,point,view->viewport()->mapToGlobal(point),button,buttons,Qt::NoModifier);
            QApplication::sendEvent(view->viewport(),&event);
        };
        send(QEvent::MouseMove,start,Qt::NoButton,Qt::NoButton);
        send(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        require(scene->mouseGrabberItem() && scene->mouseGrabberItem()->type()!=QtNodes::NodeGraphicsObject::Type,"Port press did not create QtNodes draft connection");
        const auto actual=static_cast<QtNodes::ConnectionGraphicsObject*>(scene->mouseGrabberItem())->connectionId();
        require(actual.outNodeId==from && actual.outPortIndex==output,"Port geometry selected a different actual output identity");
        for(int step=1;step<=6;++step)send(QEvent::MouseMove,start+(end-start)*step/6,Qt::NoButton,Qt::LeftButton);
        send(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
        QCoreApplication::processEvents();
        require(!scene->mouseGrabberItem(),"Port release retained draft mouse grabber");
        for(auto* item:hidden)item->show();
    }
};

int main(int argc,char** argv) {
    if(qEnvironmentVariable("QT_QPA_PLATFORM")!="offscreen")return 2;
#ifdef Q_OS_WIN
    // Offscreen's bundled font directory is absent in this developer build.
    // Supply installed fonts before QApplication constructs QtNodes geometry.
    qputenv("QT_QPA_FONTDIR",(qEnvironmentVariable("WINDIR","C:/Windows")+"/Fonts").toUtf8());
#endif
    try {
        QTemporaryDir temporary;require(temporary.isValid(),"Fixture directory absent");
        const auto path=temporary.filePath("gesture-project.json");
        domain::NumericProcessingGraph empty;empty.graph_id="main";
        desktop::NumericDraftPresentation presentation;
        presentation.application_settings={{"ui",QJsonObject{{"autosave_enabled",false},{"pipeline_auto_apply",false},{"language","en"},{"show_data_flow",true}}}};
        desktop::NumericProjectDocument project;
        require(bool(desktop::create_numeric_project(empty,presentation,project)) && bool(desktop::save_numeric_project(project,path)),"Isolated canonical fixture refused");
        auto encodedPath=path.toLocal8Bit();char name[]="numeric_editor_gesture_tests",option[]="--config";char* arguments[]={name,option,encodedPath.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory(true));
        auto& presenter=app.presenter();
        auto* model=dynamic_cast<NumericEditorGraphModel*>(presenter.state().data_flow_graph.get());
        MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if((window=qobject_cast<MainWindow*>(widget)))break;
        auto* editor=window?window->findChild<SchemeEditorWidget*>():nullptr;
        require(model && editor,"Actual application editor absent");
        window->resize(1600,900);window->show();editor->show();
        require(wait_for([&]{auto* view=editor->findChild<QtNodes::GraphicsView*>();return editor->isVisible() && view && view->viewport()->width()>300 && view->viewport()->height()>150;}),"Own offscreen editor viewport did not become ready");
        Gestures gestures(*editor,*model);

        // Establish that the oracle drives QtNodes rather than inserting edges.
        const auto network=gestures.create("NetworkInput"),networkSink=gestures.create("NetworkOutput");
        const int beforeConnect=editor->history().count();gestures.drag(network,0,networkSink,0);
        require(model->connectionExists({network,0,networkSink,0}) && editor->history().count()==beforeConnect+1,"Compatible actual port drag failed");
        std::puts("actual QtNodes compatible press/move/release PASS");

        // Reproduce the reported matching Int64 Scalar labels as an imported
        // draft. Matching individual port shapes do not make these nodes valid.
        domain::NumericProcessingGraph screenshot;screenshot.graph_id="main";
        auto invalidSource=numeric_editor::default_node("SignalGenerator");invalidSource.id=1;
        auto scalarContract=std::get<signal_contracts::NumericContract>(invalidSource.outputs[0].contract.view());
        scalarContract.clock.sample_rate=44100; // Deliberately inconsistent with supported generator geometry.
        scalarContract.dtype=signal_contracts::DType::Int64;scalarContract.semantic=signal_contracts::Semantic::Scalar;scalarContract.shape={};scalarContract.coordinates={};scalarContract.channel_order=scalarContract.channel_order.first(1);
        require(numeric_editor::set_contract(invalidSource,domain::PortDirection::Output,0,scalarContract),"Reported source contract fixture invalid");
        auto invalidConverter=numeric_editor::default_node("Converter");invalidConverter.id=14;
        require(numeric_editor::set_contract(invalidConverter,domain::PortDirection::Input,0,scalarContract),"Reported converter input fixture invalid");
        auto melContract=scalarContract;melContract.dtype=signal_contracts::DType::Float32;melContract.semantic=signal_contracts::Semantic::MelEnergy;
        melContract.shape={signal_contracts::Rank::Vector,{signal_contracts::Axis::Band,signal_contracts::Axis::None},{signal_contracts::ExtentBounds{50,50},signal_contracts::ExtentBounds{}}};
        require(numeric_editor::set_contract(invalidConverter,domain::PortDirection::Output,0,melContract),"Reported converter output fixture invalid");
        screenshot.nodes={invalidSource,invalidConverter};require(editor->load_numeric_draft(screenshot),"Imported invalid draft is not available for repair");
        gestures.drag(1,0,14,0);
        domain::ResolvedProcessingNode resolved;
        require(!domain::resolve_processing_node(invalidSource,numeric_editor::limits(),resolved) && !model->connectionExists({1,0,14,0}),"Matching scalar labels bypass invalid configured source clock");
        std::puts("reported Int64 Scalar source/converter labels: actual port drop rejected, source clock/parameter mismatch invalid REPRODUCED");
        for(const auto id:{QtNodes::NodeId{1},QtNodes::NodeId{14}}){
            auto* node=model->delegateModel<AudioNodeModel>(id);std::unique_ptr<QWidget> repair(node->parameter_widget());
            auto* reset=repair->findChild<QPushButton*>("numericResetSettings");require(reset,"Imported invalid node has no explicit repair control");
            const int before=editor->history().index();reset->click();
            require(model->node_error(*node->numeric_node()).isEmpty() && node->numeric_node()->id==id && editor->history().index()==before+1,"Explicit reset did not repair imported invalid whole node");
        }
        gestures.drag(1,0,14,0);require(model->connectionExists({1,0,14,0}),"Repaired source/converter cannot connect via actual ports");


        // This is the real shipped mono Window, not a hand-adjusted test node.
        require(editor->load_numeric_draft(numeric_default_graph()),"Shipped graph fixture refused");
        model->deleteNode(1);
        const auto signal=gestures.create("SignalGenerator");
        const auto clock=gestures.create("SampleClock");
        auto bound=*model->delegateModel<AudioNodeModel>(signal)->numeric_node();bound.clock_node=domain::NodeId(clock);
        require(model->edit_node(signal,bound),"Explicit generator clock binding failed");
        gestures.drag(signal,0,2,0);
        const auto* source=model->delegateModel<AudioNodeModel>(signal)->numeric_node();
        const auto sourceContract=std::get<signal_contracts::NumericContract>(source->outputs[0].contract.view());
        std::fprintf(stderr,"fresh SignalGenerator channels=%zu; shipped Window channels=%zu; actual edge=%d\n",sourceContract.channel_order.size(),std::get<signal_contracts::NumericContract>(model->delegateModel<AudioNodeModel>(2)->numeric_node()->inputs[0].contract.view()).channel_order.size(),model->connectionExists({signal,0,2,0}));
        require(model->connectionExists({signal,0,2,0}),"Fresh generator cannot drag-connect shipped mono Window");
        gestures.drag(signal,0,9,0);require(model->connectionExists({signal,0,9,0}),"Fresh generator cannot drag-connect shipped AudioView");
        const auto route=gestures.create("Switch");gestures.drag(signal,0,route,0);
        require(model->connectionExists({signal,0,route,0}),"Fresh generator cannot drag-connect fresh route");
        const auto sink=gestures.create("AudioView");gestures.drag(route,1,sink,0);
        require(model->connectionExists({route,1,sink,0}),"Actual drag lost stable second output identity");
        const auto mel=gestures.create("MelView");gestures.drag(signal,0,mel,0);
        require(model->connectionExists({signal,0,mel,0}) && signal_contracts::same_contract(model->delegateModel<AudioNodeModel>(mel)->numeric_node()->inputs[0].contract.view(),model->delegateModel<AudioNodeModel>(signal)->numeric_node()->outputs[0].contract.view()),"Universal Mel view must adopt generator primitive/shape");
        const auto incompatible=gestures.create("WLEDOutput");const auto validEdges=model->allConnectionIds(signal);const int beforeReject=editor->history().count();
        QSignalSpy rejected(editor,&SchemeEditorWidget::status_message);gestures.drag(signal,0,incompatible,0);
        require(!model->connectionExists({signal,0,incompatible,0}) && model->allConnectionIds(signal)==validEdges && editor->history().count()==beforeReject,"Incompatible drag changed existing routes/history");
        require(!rejected.isEmpty() && !rejected.last()[0].toString().isEmpty(),"Incompatible real drag has no visible diagnosis");

        auto* sourceModel=model->delegateModel<AudioNodeModel>(signal);std::unique_ptr<QWidget> form(sourceModel->parameter_widget());
        form->setAttribute(Qt::WA_ShowWithoutActivating);
        form->setWindowFlag(Qt::WindowDoesNotAcceptFocus,true);
        for(auto* section:form->findChildren<ui::CollapsibleSection*>())section->setExpanded(true);
        form->show();QCoreApplication::processEvents();
        const auto beforeEdit=sourceModel->save();const auto edgesBeforeEdit=model->allConnectionIds(signal);const int undoBeforeEdit=editor->history().index();
        for(const auto field: {"output0Dtype","output0Rank"}) {
            auto* choice=form->findChild<QComboBox*>(field);require(choice,"Registered source contract editor missing");
            require(choice->currentIndex()!=0,"Unsupported source choice fixture is already selected");
            QSignalSpy refusal(editor,&SchemeEditorWidget::status_message);choice->setCurrentIndex(0);QCoreApplication::processEvents();
            require(sourceModel->save()==beforeEdit && model->allConnectionIds(signal)==edgesBeforeEdit && editor->history().index()==undoBeforeEdit,"Unsupported source choice changed node/edges/undo");
            const auto* row=form->findChild<QWidget*>(QString::fromLatin1(field)+"_row");
            auto* inlineError=row?row->findChild<QLabel*>("validationMessage"):nullptr;
            const bool fieldRefusal=inlineError&&inlineError->isVisible()&&!inlineError->text().isEmpty()&&choice->property("validationError").toBool()&&choice->accessibleDescription()==inlineError->text();
            const bool editorRefusal=!refusal.isEmpty()&&!refusal.last()[0].toString().isEmpty();
            if(!fieldRefusal&&!editorRefusal)std::fprintf(stderr,"unsupported source field=%s restoredIndex=%d inline=%s visible=%d statusEvents=%lld\n",field,choice->currentIndex(),inlineError?qPrintable(inlineError->text()):"<missing>",inlineError&&inlineError->isVisible(),static_cast<long long>(refusal.size()));
            // Contract-boundary refusal is shown next to the actual field;
            // whole-node/device refusal additionally uses the editor status.
            require(fieldRefusal||editorRefusal,"Unsupported source choice has no visible refusal");
            std::fprintf(stderr,"unsupported source field=%s refusal=%s diagnostic=%s\n",field,fieldRefusal?"visible field":"editor",fieldRefusal?qPrintable(inlineError->text()):qPrintable(refusal.last()[0].toString()));
        }
        form->hide();
        // Model API must reject too: disabling options alone is insufficient.
        auto impossible=*sourceModel->numeric_node();require(numeric_editor::set_dtype(impossible,domain::PortDirection::Output,0,signal_contracts::DType::Int64),"Direct source rejection fixture contract is not valid");
        require(!domain::resolve_processing_node(impossible,numeric_editor::limits(),resolved),"Generator must reject unsupported primitive at its operation boundary");
        require(!model->edit_node(signal,impossible) && sourceModel->save()==beforeEdit && model->allConnectionIds(signal)==edgesBeforeEdit,"Direct inadmissible source edit bypasses GUI guard");

        QSignalSpy applied(&presenter,&GuiPresenter::graph_apply_finished);
        domain::NumericProcessingGraph applyGraph;QString applyError;const bool exportable=editor->numeric_draft(applyGraph,&applyError);
        if(!exportable)std::fprintf(stderr,"Actual Apply export diagnostic: %s\n",qPrintable(applyError));
        require(exportable,"Actual Apply draft is not exportable");
        auto* apply=window->findChild<QPushButton*>("applyPipeline");require(apply,"Actual Apply control missing");apply->click();
        const bool replied=wait_for([&]{return !applied.isEmpty();});
        if(!replied || !applied.last()[1].toBool())std::fprintf(stderr,"Actual Apply diagnostic: reply=%d error=%s status=%s\n",replied,replied?qPrintable(applied.last()[2].toString()):"<no reply>",qPrintable(presenter.apply_status()));
        require(replied && applied.last()[1].toBool(),"Actual Apply rejects newly created connected generator");
        require(!window->findChild<QPushButton*>("pipelineRun") && !window->findChild<QPushButton*>("pipelineStop"),"Obsolete Run/Stop controls returned");
        int delivered=0;std::uint64_t firstSequence=0,lastSequence=0;float peak=0;bool nonzero=false,progressing=false;
        QObject::connect(&presenter,&GuiPresenter::numeric_frame_ready,editor,[&](const pipeline::DeliveredNumericFrame& frame) {
            if(!frame.context || frame.context->endpoint.node!=9 || !frame.frame || frame.status!=signal_contracts::Status::ok || !frame.frame.view().stamp.available)return;
            const auto data=frame.frame.view();const auto* numeric=std::get_if<signal_contracts::NumericView>(&data.payload);if(!numeric)return;
            for(std::size_t i=0;i<numeric->actual[0];++i){float value=0;require(signal_contracts::load_element(*numeric,{0,i,0},value)==signal_contracts::Status::ok && std::isfinite(value),"Continuous source delivered invalid PCM");peak=std::max(peak,std::abs(value));}
            nonzero|=peak>.0001f;const auto sequence=data.stamp.sequence;
            if(!delivered)firstSequence=sequence;else progressing|=sequence>lastSequence;
            lastSequence=sequence;++delivered;
        });
        // GUI taps follow exposed graph instances; an editor-only fixture has
        // no implicit subscription. Observe the real raw PCM graph explicitly.
        auto* raw=window->workspaceDock("rawSamplesDock");require(raw,"Continuous source graph dock missing");raw->toggleView(true);raw->setAsCurrentTab();
        const bool continuous=wait_for([&]{return delivered>=3 && nonzero && progressing;});
        if(!continuous)std::fprintf(stderr,"Continuous source diagnostic: delivered=%d peak=%.9g firstSequence=%llu lastSequence=%llu exposed=%d closed=%d\n",delivered,double(peak),(unsigned long long)firstSequence,(unsigned long long)lastSequence,raw->property("renderExposed").toBool(),raw->isClosed());
        require(continuous,"Continuous Apply has no progressing nonzero PCM sink frames");
        std::puts("fresh mono generator/Window/route actual gestures, rejected edits/diagnostics, Apply and continuous synthetic execution PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
