#pragma once
#include "shipped_examples_probe.h"
#include <algorithm>
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/graph_panel.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/editor_geometry.h>
#include <QGraphicsView>
#include <QMouseEvent>
#include <desktop/numeric_project_document.h>
#include <barelief/ui_text.h>
#include <barelief/abstract/abs.h>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <QFile>
#include <QJsonDocument>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QAction>
#include <QStatusBar>
#include <QComboBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QThread>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <memory>
#include <optional>
#include <functional>
#include <vector>

namespace shipped_gui_probe {
using namespace pipeline;
using namespace signal_contracts;

template<class Predicate> inline bool wait(const Predicate& predicate,int timeout=6000) {
    QElapsedTimer timer;
    timer.start();
    while(timer.elapsed()<timeout) {
        QCoreApplication::processEvents();
        if(predicate())return true;
        QThread::msleep(2);
    }
    return predicate();
}
struct TapGuard {
    NumericEngine& engine;
    std::uint64_t token;
    bool always=false;
    ~TapGuard(){if(token||always)engine.unsubscribe_numeric_tap(token);}
};
struct Stage {
    EndpointIdentity endpoint;
    Rank rank;
    DType dtype;
    std::size_t width;
    unsigned count=0;
    std::uint64_t first=0,last=0;
    bool positive=false;
    std::uint64_t producer_epoch=0,generation=0;
    unsigned received=0,unavailable=0,exhausted=0;
    bool history=false,optional=false,missing=false;
};
struct RawWindowObservation {
    std::mutex mutex;
    unsigned available=0,unavailable=0,exhausted=0;
    std::uint64_t first=0,last=0;
    void observe(const DeliveredNumericFrame& frame) {
        std::lock_guard lock(mutex);
        if(!frame.frame || !frame.frame.view().stamp.available) {
            ++unavailable;
            if(frame.status==Status::exhausted)++exhausted;
            return;
        }
        if(!available)first=frame.frame.view().stamp.sequence;
        last=frame.frame.view().stamp.sequence;
        ++available;
    }
    void report() {
        std::lock_guard lock(mutex);
        std::printf(" raw102=%u/%llu..%llu(u%u,x%u)",available,(unsigned long long)first,
                    (unsigned long long)last,unavailable,exhausted);
    }
};
struct RawScalarObservation {
    struct Entry {
        EndpointIdentity endpoint;
        DType dtype;
        unsigned count=0;
        std::uint64_t producer_epoch=0,generation=0,first=0,last=0;
        bool optional=false,missing=false;
    };
    mutable std::mutex mutex;
    std::vector<Entry> entries;
    bool bad=false;
    unsigned unavailable=0,exhausted=0;
    template<class T> static bool finite_scalar(const NumericView& view) {
        T value=0;
        return load_element(view,{0,0,0},value)==Status::ok&&std::isfinite(double(value));
    }
    static bool valid_value(const NumericView& view,DType dtype) {
        if(!view.contract||view.contract->dtype!=dtype||view.contract->shape.rank!=Rank::Scalar)return false;
        switch(dtype) {
        case DType::Float32:return finite_scalar<float>(view);
        case DType::Float64:return finite_scalar<double>(view);
        case DType::UInt32:return finite_scalar<std::uint32_t>(view);
        case DType::UInt64:return finite_scalar<std::uint64_t>(view);
        default:return false;
        }
    }
    void sequence(Entry& entry,const FrameStamp& stamp) {
        if(entry.count&&(entry.producer_epoch!=stamp.producer_epoch||entry.generation!=stamp.generation))entry.count=0;
        if(entry.count&&stamp.sequence<entry.last){bad=true;return;}
        if(entry.count&&stamp.sequence==entry.last)return;
        if(!entry.count)entry.first=stamp.sequence;
        entry.producer_epoch=stamp.producer_epoch;entry.generation=stamp.generation;
        entry.last=stamp.sequence;++entry.count;
    }
    bool readable(const DeliveredNumericFrame& frame) {
        if(frame.status==Status::exhausted)++exhausted;
        if(frame.status!=Status::ok||!frame.frame||!frame.frame.view().stamp.available){++unavailable;return false;}
        return true;
    }
    void observe(const DeliveredNumericFrame& frame) {
        std::lock_guard lock(mutex);
        if(!frame.context)return;
        auto entry=std::find_if(entries.begin(),entries.end(),[&](const auto& item){return item.endpoint==frame.context->endpoint;});
        if(entry==entries.end())return;
        if(!readable(frame)) {
            entry->missing|=frame.status==Status::unavailable ||
                (frame.status==Status::ok&&frame.frame&&!frame.frame.view().stamp.available);
            return;
        }
        const auto frame_view=frame.frame.view();
        const auto* view=std::get_if<NumericView>(&frame_view.payload);
        if(!view||!valid_value(*view,entry->dtype)){bad=true;return;}
        sequence(*entry,frame_view.stamp);
    }
    bool complete() const {
        std::lock_guard lock(mutex);
        return !bad&&std::all_of(entries.begin(),entries.end(),[](const auto& entry){return (entry.optional&&entry.missing)||(entry.count>=5&&entry.last>entry.first);});
    }
    void report() const {
        std::lock_guard lock(mutex);
        for(const auto& entry:entries)std::printf(" scalar_raw%d:%u=%u/%llu..%llu",int(entry.endpoint.node),unsigned(entry.endpoint.port),entry.count,(unsigned long long)entry.first,(unsigned long long)entry.last);
        std::printf(" scalar_raw_unavailable=%u exhausted=%u",unavailable,exhausted);
        if(bad)std::printf(" scalar_raw_invalid");
    }
};
struct AppliedGuiTurn {
    std::uint64_t revision=0,epoch=0;
    bool settled=false;
    QObject witness;
    void observe(GuiPresenter& presenter,std::uint64_t before) {
        const auto current=presenter.client().snapshot();
        if(current.graph_revision<=before)return;
        revision=current.graph_revision;epoch=current.epoch;settled=false;
        QMetaObject::invokeMethod(&witness,[this,&presenter]{
            const auto current=presenter.client().snapshot();
            settled=current.graph_revision==revision&&current.epoch==epoch;
        },Qt::QueuedConnection);
    }
    bool matches(const EngineSnapshot& current) const {
        return settled&&current.graph_revision==revision&&current.epoch==epoch;
    }
};
class ShippedGuiProbe {
    CoreApplication& app;
    MainWindow* window;
    const char* label;
    GuiPresenter& presenter;
    domain::NumericProcessingGraph expected;
    std::uint64_t revision=0,epoch=0;
    std::vector<Stage> stages;
    std::vector<EndpointIdentity> endpoints;
    QObject witness;
    bool bad=false;
    std::uint64_t ordinal=0;
    std::shared_ptr<RawWindowObservation> rawWindow;
    std::uint64_t rawWindowTap=0;
    std::optional<TapGuard> rawGuard;
    bool has_history=false;
    std::size_t prepared_values=0;
    std::shared_ptr<RawScalarObservation> rawScalars=std::make_shared<RawScalarObservation>();
    std::optional<TapGuard> scalarGuard;

    bool apply_ready(std::uint64_t before) const {
        return presenter.client().snapshot().graph_revision>before&&!presenter.graph_request_pending()&&
            presenter.client().snapshot().statuses.value("analyzer").state==State::Running;
    }
    bool apply_graph() {
        const auto before=presenter.client().snapshot().graph_revision;
        AppliedGuiTurn gui;
        QObject::connect(&presenter,&GuiPresenter::processing_state_changed,&gui.witness,[&]{gui.observe(presenter,before);});
        presenter.apply_pipeline_graph({});
        if(!wait([&]{return apply_ready(before)&&gui.matches(presenter.client().snapshot());}))return false;
        if(!presenter.applied_numeric_graph())return false;
        expected=*presenter.applied_numeric_graph();
        revision=presenter.client().snapshot().graph_revision;
        epoch=presenter.client().snapshot().epoch;
        return true;
    }
    bool record_prepared_value(domain::PreparationValueCapability capability,Status status,const NumericLiteral& value) {
        if(status==Status::unavailable&&capability.kind==domain::PreparationValueKind::Routed)return false;
        if(status!=Status::ok){bad=true;return true;}
        bad|=!std::visit([](auto scalar){return std::isfinite(double(scalar));},value);
        ++prepared_values;return true;
    }
    bool prepared_output(const auto& node,domain::PortId port) {
        const auto capability=domain::preparation_value_capability(node,port);
        if(!capability)return false;
        NumericLiteral value;const auto status=domain::resolve_preparation_value(expected,node.id,port,value);
        return record_prepared_value(capability,status,value);
    }
    bool prepared_input(const auto& node,const auto& port) {
        const auto edge=std::find_if(expected.edges.begin(),expected.edges.end(),[&](const auto& e){return e.to==node.id&&e.input_port==port.id;});
        if(edge==expected.edges.end())return false;
        const auto producer=std::find_if(expected.nodes.begin(),expected.nodes.end(),[&](const auto& n){return n.id==edge->from;});
        return producer!=expected.nodes.end()&&prepared_output(*producer,edge->output_port);
    }
    void add_port(const auto& node,const auto& port,bool view) {
        if(!port.contract)return;
        if(view?prepared_input(node,port):prepared_output(node,port.id))return;
        const auto contract=port.contract.view();
        const auto* numeric=std::get_if<NumericContract>(&contract);
        if(!numeric)return;
        EndpointIdentity endpoint{expected.graph_id,node.id,view?domain::PortDirection::Input:domain::PortDirection::Output,port.id};
        stages.push_back({endpoint,numeric->shape.rank,numeric->dtype,
            numeric->shape.extents[numeric->shape.rank==Rank::Matrix?1:0].maximum});
        stages.back().history=numeric->semantic==Semantic::History;
        stages.back().optional=shipped_probe::allows_missing(expected,node.id,endpoint.direction,port.id,expected.nodes.size());
        endpoints.push_back(endpoint);
        if(numeric->shape.rank==Rank::Scalar) {
            rawScalars->entries.push_back({endpoint,numeric->dtype});
            rawScalars->entries.back().optional=stages.back().optional;
        }
    }
    void collect_stages() {
        for(const auto& node:expected.nodes) {
            const auto kind=domain::node_kind(node.kind);
            const bool view=kind&&domain::node_role(*kind)==domain::NodeRole::Sink;
            if(node.kind=="SampleClock"||node.kind=="PcmResampler"||(node.kind=="AudioSource" && node.id!=1))continue;
            const auto ports=view?domain::data_inputs(node):std::span<const domain::ProcessingPort>(node.outputs);
            for(const auto& port:ports)add_port(node,port,view);
        }
    }
    void fail(const Stage& stage,const DeliveredNumericFrame& frame,const char* reason) {
            if(!bad)std::fprintf(stderr,"GUI %s stage%d:%u invalid: %s producer=%llu/%llu sequence=%llu prior=%llu/%llu/%llu\n",label,int(stage.endpoint.node),unsigned(stage.endpoint.port),reason,
                (unsigned long long)frame.frame.view().stamp.producer_epoch,(unsigned long long)frame.frame.view().stamp.generation,(unsigned long long)frame.frame.view().stamp.sequence,
                (unsigned long long)stage.producer_epoch,(unsigned long long)stage.generation,(unsigned long long)stage.last);
            bad=true;
    }
    template<class T> bool scalar_energy(Stage& stage,const DeliveredNumericFrame& frame,const NumericView& numeric,
                                        const char* reason,double& energy,bool square=false) {
        T value=0;
        if(load_element(numeric,{0,0,0},value)!=Status::ok || (square&&!std::isfinite(value))) {
            fail(stage,frame,reason);
            return false;
        }
        energy=square?value*value:double(value);
        return true;
    }
    template<class Value=float> bool float_energy(Stage& stage,const DeliveredNumericFrame& frame,const NumericView& numeric,double& energy) {
        if(numeric.planes.size()!=1 || numeric.actual[0]==0 || numeric.actual[0]>stage.width ||
           (stage.rank==Rank::Matrix && numeric.actual[0]!=stage.width)) {
            fail(stage,frame,"Float32 geometry");
            return false;
        }
        for(std::size_t i=0;i<numeric.actual[0];++i) {
            Value value=0;
            if(load_element(numeric,{0,i,0},value)!=Status::ok||!std::isfinite(value)) {
                fail(stage,frame,"Float32 read or finiteness");
                return false;
            }
            energy+=double(value)*value;
        }
        return true;
    }
    bool energy_of(Stage& stage,const DeliveredNumericFrame& frame,const NumericView& numeric,double& energy) {
        switch(stage.dtype) {
        case DType::UInt8:return float_energy<std::uint8_t>(stage,frame,numeric,energy);
        case DType::UInt16:return float_energy<std::uint16_t>(stage,frame,numeric,energy);
        case DType::UInt64:return scalar_energy<std::uint64_t>(stage,frame,numeric,"UInt64 read",energy);
        case DType::UInt32:return scalar_energy<std::uint32_t>(stage,frame,numeric,"UInt32 read",energy);
        case DType::Float64:return scalar_energy<double>(stage,frame,numeric,"Float64 read or finiteness",energy,true);
        case DType::Float32:return float_energy(stage,frame,numeric,energy);
        default:fail(stage,frame,"unsupported primitive dtype");return false;
        }
    }
    void sequence(Stage& stage,const DeliveredNumericFrame& frame,double energy) {
        const auto view=frame.frame.view();
        // Sequence belongs to a producer/generation; restarting one requires five new frames.
        if(stage.count && (view.stamp.producer_epoch!=stage.producer_epoch || view.stamp.generation!=stage.generation)) {
            stage.count=0;stage.positive=false;
        }
        if(stage.count && view.stamp.sequence<stage.last) {
            fail(stage,frame,"sequence regressed within producer generation");return;
        }
        stage.producer_epoch=view.stamp.producer_epoch;stage.generation=view.stamp.generation;
        if(!stage.count)stage.first=view.stamp.sequence;
        stage.last=view.stamp.sequence;++stage.count;stage.positive|=energy>1e-12;
    }
    void available(Stage& stage,const DeliveredNumericFrame& frame) {
        const auto view=frame.frame.view();
        const auto* numeric=std::get_if<NumericView>(&view.payload);
        if(!numeric||numeric->contract->dtype!=stage.dtype) {fail(stage,frame,"payload or dtype mismatch");return;}
        double energy=0;
        if(!energy_of(stage,frame,*numeric,energy))return;
        sequence(stage,frame,energy);
    }
    void observe(const DeliveredNumericFrame& frame,bool compact) {
        if(!frame.context||frame.delivery.graph_revision!=revision||frame.delivery.epoch!=epoch)return;
        auto stage=std::find_if(stages.begin(),stages.end(),[&](const auto& item){return item.endpoint==frame.context->endpoint;});
        if(stage==stages.end() || stage->history!=compact)return;
        ++stage->received;
        if(!frame.frame || !frame.frame.view().stamp.available) {
            ++stage->unavailable;
            stage->missing|=frame.status==Status::unavailable ||
                (frame.status==Status::ok&&frame.frame&&!frame.frame.view().stamp.available);
            if(frame.status==Status::exhausted)++stage->exhausted;
            return;
        }
        available(*stage,frame);
    }
    std::uint64_t declared_history_capacity(const EndpointIdentity& endpoint) const {
        const auto node=std::find_if(expected.nodes.begin(),expected.nodes.end(),[&](const auto& value){return value.id==endpoint.node;});
        if(node==expected.nodes.end())return 0;
        const auto& ports=endpoint.direction==domain::PortDirection::Input?node->inputs:node->outputs;
        const auto port=std::find_if(ports.begin(),ports.end(),[&](const auto& value){return value.id==endpoint.port;});
        if(port==ports.end())return 0;
        const auto contract=port->contract.view();const auto* numeric=std::get_if<NumericContract>(&contract);
        return numeric&&numeric->semantic==Semantic::History?numeric->shape.extents[0].maximum:0;
    }
    void observe_empty_history(const DeliveredNumericHistoryRow& row,std::uint64_t capacity) {
        if((row.event!=NumericHistoryEvent::Empty&&row.event!=NumericHistoryEvent::Unavailable)||row.filled_count!=0||row.capacity!=capacity)return;
        const auto endpoint=row.compact.context->endpoint;
        const auto stage=std::find_if(stages.begin(),stages.end(),[&](const auto& value){return value.endpoint==endpoint;});
        if(stage!=stages.end())stage->missing=true;
    }
    void observe_history(const DeliveredNumericHistoryRow& row) {
        if(!row.compact.context || row.compact.delivery.graph_revision!=revision || row.compact.delivery.epoch!=epoch)return;
        const auto capacity=declared_history_capacity(row.compact.context->endpoint);
        if(row.event!=NumericHistoryEvent::Row){observe_empty_history(row,capacity);return;}
        if(!capacity || row.capacity!=capacity || row.filled_count!=std::min(row.accepted_row_ordinal,capacity)) {
            if(!bad)std::fprintf(stderr,"GUI %s History invalid: node=%d capacity=%llu filled=%llu ordinal=%llu\n",label,
                row.compact.context?int(row.compact.context->endpoint.node):-1,(unsigned long long)row.capacity,
                (unsigned long long)row.filled_count,(unsigned long long)row.accepted_row_ordinal);
            bad=true;
        }
        ordinal=std::max(ordinal,row.accepted_row_ordinal);
        observe(row.compact,true);
    }
    void connect_observers() {
        QObject::connect(&presenter,&GuiPresenter::numeric_frame_ready,&witness,[&](const auto& frame){observe(frame,false);});
        QObject::connect(&presenter,&GuiPresenter::numeric_history_ready,&witness,[&](const auto& row){observe_history(row);});
    }
    void start_raw_window() {
        rawWindow=std::make_shared<RawWindowObservation>();
        if(std::any_of(stages.begin(),stages.end(),[](const auto& stage){return stage.endpoint.node==102 && stage.endpoint.direction==domain::PortDirection::Output && stage.endpoint.port==0;}))
            rawWindowTap=app.pipeline().subscribe_numeric_tap({{expected.graph_id,102,domain::PortDirection::Output,0}},
                [raw=rawWindow,revision=revision,epoch=epoch](const DeliveredNumericFrame& frame) {
                    if(frame.delivery.graph_revision==revision && frame.delivery.epoch==epoch)raw->observe(frame);
                },{});
        rawGuard.emplace(app.pipeline(),rawWindowTap);
    }
    void start_raw_scalars() {
        if(rawScalars->entries.empty())return;
        std::vector<EndpointIdentity> selected;
        for(const auto& entry:rawScalars->entries)selected.push_back(entry.endpoint);
        const auto token=app.pipeline().subscribe_numeric_tap(std::move(selected),
            [raw=rawScalars,revision=revision,epoch=epoch](const DeliveredNumericFrame& frame) {
                if(frame.delivery.graph_revision==revision&&frame.delivery.epoch==epoch)raw->observe(frame);
            },{});
        if(!token)bad=true;
        scalarGuard.emplace(app.pipeline(),token);
    }
    bool complete() const {
        for(const auto& stage:stages) {
            if(stage.optional&&stage.missing)continue;
            if(stage.rank==Rank::Scalar){if(!stage.count)return false;}
            else if(stage.count<5||stage.last<=stage.first||!stage.positive)return false;
        }
        return rawScalars->complete()&&(!has_history||ordinal>=5);
    }
    void report() {
        std::printf("GUI %s own engine epoch=%llu revision=%llu History=%llu prepared=%zu",label,(unsigned long long)epoch,
                    (unsigned long long)revision,(unsigned long long)ordinal,prepared_values);
        for(const auto& stage:stages)std::printf(" stage%d:%u=%u/%llu(r%u,u%u,x%u,p%d)",int(stage.endpoint.node),
            unsigned(stage.endpoint.port),stage.count,(unsigned long long)stage.last,stage.received,stage.unavailable,stage.exhausted,int(stage.positive));
        if(rawWindowTap)rawWindow->report();
        rawScalars->report();
    }
public:
    ShippedGuiProbe(CoreApplication& app,MainWindow* window,const char* label)
        :app(app),window(window),label(label),presenter(app.presenter()){}
    bool run() {
        if(!apply_graph())return false;
        collect_stages();
        if(stages.empty())return false;
        connect_observers();start_raw_window();start_raw_scalars();
        presenter.set_editor_numeric_endpoints(std::move(endpoints));
        has_history=std::any_of(expected.nodes.begin(),expected.nodes.end(),[](const auto& n){return n.kind=="Spectrogram";});
        const bool delivered=wait([&]{return bad||complete();});
        return finish(delivered);
    }
    bool finish(bool delivered) {
        presenter.set_editor_numeric_endpoints({});
        bool good=delivered&&!bad&&complete();
        report();
        auto* validation=window->findChild<QLabel*>("pipelineValidation");
        good&=validation&&validation->toolTip().isEmpty();
        if(!good)std::fprintf(stderr,"GUI completion delivered=%d complete=%d bad=%d validation=%s\n",int(delivered),int(complete()),int(bad),validation?qPrintable(validation->toolTip()):"missing");
        std::printf(" %s\n",good?"PASS":"FAIL");
        return good;
    }
};
} // namespace shipped_gui_probe
inline bool measure_shipped_gui(CoreApplication& app,MainWindow* window,const char* label) {
    return shipped_gui_probe::ShippedGuiProbe(app,window,label).run();
}

namespace shipped_gui_probe {
struct SelectorTransition {
    unsigned fft,generator,capture;
    std::uint64_t revision,ordinal;
};
struct DiagnosticGuard {
    std::function<void()> print;
    ~DiagnosticGuard(){print();}
};
class StartupSelectorProbe {
    CoreApplication& app;
    MainWindow* window;
    bool unavailable;
    GuiPresenter& presenter;
    NumericEditorGraphModel* model=nullptr;
    AudioNodeModel* source=nullptr;
    AudioNodeModel* selector=nullptr;
    std::unique_ptr<QWidget> sourceDiagnostics;
    QWidget* card=nullptr;
    QLabel* values=nullptr;
    QLabel* first=nullptr;
    SchemeEditorWidget* editor=nullptr;
    QtNodes::DataFlowGraphicsScene* inspectorScene=nullptr;
    std::unique_ptr<QWidget> selectorDiagnostics;
    QObject witness;
    bool bad=false;
    int selected=0;
    std::uint64_t revision=0,epoch=0,ordinal=0,history_sequence=0,transition_after=0;
    unsigned generator_frames=0,capture_frames=0,selected_frames=0,unavailable_frames=0,fft_frames=0;
    ads::CDockWidget* fftDock=nullptr;
    int phase=0;
    std::optional<DiagnosticGuard> diagnostic;
    std::optional<TapGuard> rawGuard;
    SpectrogramGraph* surface=nullptr;
    GraphPanel* surfacePanel=nullptr;

    bool source_card() {
        model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
        if(!model)return false;
        source=model->delegateModel<AudioNodeModel>(1);
        selector=model->delegateModel<AudioNodeModel>(200);
        sourceDiagnostics.reset(source?source->preview_widget():nullptr);
        card=sourceDiagnostics.get();
        values=card?card->findChild<QLabel*>("previewValues"):nullptr;
        first=card?card->findChild<QLabel*>("previewExactValue"):nullptr;
        return selector&&card&&values&&first&&!window->findChild<QPushButton*>("pipelineRun")&&
               !window->findChild<QPushButton*>("pipelineStop")&&!window->findChild<QCheckBox*>("pipelineAutoApply");
    }
    bool editor_fixture() {
        auto* body=selector->embeddedWidget();
        editor=window->findChild<SchemeEditorWidget*>();
        if(!editor||selector->selected_input()!=1||body->findChild<QWidget*>("pipelineDataStatus"))return false;
        if(selector->numeric_node()->inputs.size()!=4 || domain::data_outputs(*selector->numeric_node()).size()!=2 || selector->grouped_channels(QtNodes::PortType::In)!=2)return false;
        inspectorScene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();
        if(!inspectorScene)return false;
        emit inspectorScene->nodeSelected(1);
        selectorDiagnostics.reset(selector->preview_widget());
        const auto before=presenter.client().snapshot().graph_revision;
        if(!selector->select_input(0))return false;
        return wait([&]{return !presenter.graph_request_pending()&&presenter.client().snapshot().graph_revision>before;});
    }
    bool click_view(QGraphicsView* view,auto* node,EditorNodeGeometry* geometry,QtNodes::PortIndex port,domain::PortId id) {
        geometry->recomputeSize(200);
        view->centerOn(node->mapToScene(geometry->selectorButtonRect(200,port).center()));
        // Embedded-form layout can change node height; derive the final pin centre after delivery.
        QCoreApplication::processEvents();geometry->recomputeSize(200);
        const auto point=node->mapToScene(geometry->selectorButtonRect(200,port).center());
        view->centerOn(point);
        const auto local=view->mapFromScene(point);
        const QPointF global=view->viewport()->mapToGlobal(local);
        QMouseEvent press(QEvent::MouseButtonPress,QPointF(local),global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease,QPointF(local),global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(view->viewport(),&press);QApplication::sendEvent(view->viewport(),&release);
        if(selector->selected_input()!=id)std::fprintf(stderr,"selector click miss input=%u actual=%u local=%d,%d viewport=%dx%d items=%lld\n",unsigned(id),unsigned(selector->selected_input()),local.x(),local.y(),view->viewport()->width(),view->viewport()->height(),static_cast<long long>(view->items(local).size()));
        return selector->selected_input()==id;
    }
    bool click_input(domain::PortId id) {
        const auto views=editor->findChildren<QGraphicsView*>();
        for(auto* view:views) {
            auto* scene=dynamic_cast<QtNodes::BasicGraphicsScene*>(view->scene());
            if(!scene)continue;
            auto* node=scene->nodeGraphicsObject(200);
            auto* geometry=dynamic_cast<EditorNodeGeometry*>(&scene->nodeGeometry());
            if(!node||!geometry)continue;
            const auto port=selector->port_index(QtNodes::PortType::In,id*2);
            if(port==QtNodes::InvalidPortIndex)return false;
            return click_view(view,node,geometry,port,id);
        }
        return false;
    }
    bool prepared_lane(const domain::ConfiguredProcessingNode& node,domain::PortId port) const {
        const auto graph=presenter.applied_numeric_graph();
        if(!graph)return false;
        NumericLiteral value;
        return domain::resolve_preparation_value(*graph,node.id,port,value)==Status::ok&&
            std::visit([](auto v){return std::isfinite(static_cast<double>(v))&&v>0;},value);
    }
    bool live_lane(const domain::ConfiguredProcessingNode& node,std::size_t lane,
                   const QString& count,const QString& value) const {
        const auto& port=node.outputs[lane];
        const auto contract=port.contract.view();
        const auto* numeric=std::get_if<NumericContract>(&contract);
        if(numeric&&numeric->shape.rank==Rank::Scalar&&numeric->dtype==DType::UInt32) {
            return count==QStringLiteral("—")&&value==QStringLiteral("—")&&prepared_lane(node,port.id);
        }
        bool countOk=false,valueOk=false;
        const auto n=count.toULongLong(&countOk);const auto v=value.toDouble(&valueOk);
        return countOk&&n>0&&valueOk&&std::isfinite(v);
    }
    bool live_card(QWidget* widget,const domain::ConfiguredProcessingNode& node) const {
        const auto* count=widget?widget->findChild<QLabel*>("previewValues"):nullptr;
        const auto* first=widget?widget->findChild<QLabel*>("previewExactValue"):nullptr;
        if(!widget||!count||!first||!widget->property("previewActivity").toString().isEmpty())return false;
        const auto counts=count->text().split('\n'),values=first->text().split('\n');
        if(counts.size()!=qsizetype(node.outputs.size())||values.size()!=qsizetype(node.outputs.size()))return false;
        for(qsizetype lane=0;lane<counts.size();++lane)
            if(!live_lane(node,std::size_t(lane),counts[lane],values[lane]))return false;
        return true;
    }
    void print_diagnostic() {
        const auto snapshot=presenter.client().snapshot();
        domain::NumericProcessingGraph selectorDraft;QString selectorDraftError;
        const bool validDraft=editor->numeric_draft(selectorDraft,&selectorDraftError);
        std::fprintf(stderr,"selector draft group=%u valid=%d error=%s undo=%d fft_frames=%u\n",unsigned(selector->selected_input()),int(validDraft),qPrintable(selectorDraftError),editor->history().count(),fft_frames);
        print_applied_group();
        std::fprintf(stderr,"selector diagnostic phase=%d selected=%d bad=%d unavailable=%d rev=%llu current=%llu state=%d gen=%u cap=%u chosen=%u unavailable_frames=%u ordinal=%llu seq=%llu cardActivity=%s values=%s first=%s apply=%s\n",phase,selected,int(bad),int(unavailable),static_cast<unsigned long long>(revision),static_cast<unsigned long long>(snapshot.graph_revision),int(snapshot.statuses.value("analyzer").state),generator_frames,capture_frames,selected_frames,unavailable_frames,static_cast<unsigned long long>(ordinal),static_cast<unsigned long long>(history_sequence),qPrintable(card->property("previewActivity").toString()),qPrintable(values->text()),qPrintable(first->text()),qPrintable(presenter.apply_status()));
    }
    void print_applied_group() {
        const auto active=presenter.applied_numeric_graph();
        if(!active)return;
        for(const auto& node:active->nodes)if(node.id==200)
            for(const auto& p:std::get<domain::TransformProcessingParameters>(node.parameters).values)if(p.key=="selected_input")
                std::fprintf(stderr,"selector applied group=%llu\n",(unsigned long long)std::visit([](auto value){return std::uint64_t(value);},std::get<NumericLiteral>(p.value)));
    }
    bool valid_delivery(const DeliveredNumericFrame& frame) const {
        const auto current=presenter.client().snapshot();
        return frame.context&&frame.frame&&frame.delivery.graph_revision==current.graph_revision&&
               frame.delivery.epoch==current.epoch&&
               (revision?frame.delivery.graph_revision==revision:frame.delivery.graph_revision>transition_after);
    }
    bool numeric_energy(const NumericView& numeric,double& energy) {
        for(std::size_t p=0;p<1;++p)for(std::size_t i=0;i<numeric.actual[0];++i) {
            float x=0;
            if(load_element(numeric,{p,i,0},x)!=Status::ok||!std::isfinite(x)){bad=true;return false;}
            energy+=double(x)*x;
        }
        return true;
    }
    void observe_fft(const DeliveredNumericFrame& frame) {
        const auto view=frame.frame.view();
        const auto* numeric=std::get_if<NumericView>(&view.payload);
        if(view.stamp.available&&numeric&&numeric->actual[0]>0) {
            double energy=0;
            if(!numeric_energy(*numeric,energy))return;
            if(energy>0)++fft_frames;
        }
    }
    void observe_pcm(const DeliveredNumericFrame& frame) {
        const auto endpoint=frame.context->endpoint;
        const auto view=frame.frame.view();
        if(!view.stamp.available){if(endpoint.node==200)++unavailable_frames;return;}
        const auto* numeric=std::get_if<NumericView>(&view.payload);
        if(!numeric||numeric->contract->semantic!=Semantic::PCM||numeric->contract->dtype!=DType::Float32||
           numeric->planes.size()!=1||numeric->actual[0]==0||numeric->actual[0]>numeric->contract->shape.extents[0].maximum){bad=true;return;}
        double energy=0;
        if(!numeric_energy(*numeric,energy))return;
        record_pcm(endpoint.node,energy);
    }
    void record_pcm(domain::NodeId node,double energy) {
        if(node==1)++generator_frames;
        if(node==13)++capture_frames;
        if(node==200){++selected_frames;bad|=selected==0?energy<=0.002:(unavailable?energy!=0:energy<=0||energy>=0.002);}
    }
    void observe(const DeliveredNumericFrame& frame) {
        if(!valid_delivery(frame))return;
        const auto endpoint=frame.context->endpoint;
        if(endpoint.node==10&&endpoint.direction==PortDirection::Input&&endpoint.port==0){observe_fft(frame);return;}
        if(endpoint.direction!=PortDirection::Output||endpoint.port!=0)return;
        if(endpoint.node!=1&&endpoint.node!=13&&endpoint.node!=200)return;
        observe_pcm(frame);
    }
    void observe_history(const DeliveredNumericHistoryRow& row) {
        const auto current=presenter.client().snapshot();
        if(row.event!=NumericHistoryEvent::Row||!row.compact.context||row.compact.context->endpoint.node!=7||
           row.compact.context->endpoint.direction!=PortDirection::Output||row.compact.delivery.graph_revision!=current.graph_revision||
           row.compact.delivery.epoch!=current.epoch||
           (revision?row.compact.delivery.graph_revision!=revision:row.compact.delivery.graph_revision<=transition_after))return;
        bad|=row.accepted_row_ordinal<=ordinal||row.filled_count!=std::min(row.accepted_row_ordinal,std::uint64_t{512})||row.capacity!=512;
        const auto sequence=row.compact.frame.view().stamp.sequence;
        bad|=sequence<=history_sequence;
        ordinal=row.accepted_row_ordinal;history_sequence=sequence;
    }
    void connect_observers() {
        QObject::connect(&presenter,&GuiPresenter::numeric_frame_ready,&witness,
            [&](const auto& frame){if(frame.context&&frame.context->endpoint.node==10)observe(frame);});
        const auto rawTap=app.pipeline().subscribe_numeric_tap(
            {{"main",1,PortDirection::Output,0},{"main",13,PortDirection::Output,0},{"main",200,PortDirection::Output,0},{"main",7,PortDirection::Output,0}},
            [&](DeliveredNumericFrame frame){QMetaObject::invokeMethod(&witness,[&,frame]{observe(frame);},Qt::QueuedConnection);},
            [&](DeliveredNumericHistoryRow row){QMetaObject::invokeMethod(&witness,[&,row]{observe_history(row);},Qt::QueuedConnection);});
        rawGuard.emplace(app.pipeline(),rawTap,true);
    }
    void subscribe() {
        revision=presenter.client().snapshot().graph_revision;
        epoch=presenter.client().snapshot().epoch;
    }
    bool initial_frames() {
        return selected_frames>=5&&generator_frames>=5&&ordinal>=5&&(!unavailable?capture_frames>=5:true)&&
               card->property("previewActivity").toString().isEmpty()&&live_card(card,*source->numeric_node());
    }
    bool applied_choice(int choice) const {
        const auto active=presenter.applied_numeric_graph();
        if(!active)return false;
        for(const auto& node:active->nodes)if(node.id==200)
            for(const auto& parameter:std::get<domain::TransformProcessingParameters>(node.parameters).values)
                if(parameter.key=="selected_input")return std::visit([choice](auto value){return value==choice;},std::get<NumericLiteral>(parameter.value));
        return false;
    }
    bool request_choice(int choice,int transition) {
        selected=choice;selected_frames=0;unavailable_frames=0;
        if(transition==1)editor->history().undo();
        else if(transition==2)editor->history().redo();
        else if(!click_input(domain::PortId(choice)))return false;
        return true;
    }
    bool wait_choice(int choice,const SelectorTransition& prior) {
        return wait([&]{return presenter.client().snapshot().graph_revision>prior.revision&&
                       !presenter.graph_request_pending()&&applied_choice(choice);});
    }
    bool wait_transition_frames(int choice,const SelectorTransition& prior) {
        if(unavailable&&choice==1) {
            if(!wait([&]{return bad||(selected_frames>=5&&ordinal>prior.ordinal&&generator_frames>prior.generator+5);})||bad)return false;
            const auto capture=presenter.client().snapshot().statuses.value("capture");
            return capture.state==State::Failed&&capture.error.contains("Injected loopback unavailable");
        }
        return wait([&]{return bad||(selected_frames>=5&&fft_frames>prior.fft&&ordinal>prior.ordinal&&
                    generator_frames>prior.generator+5&&(unavailable||capture_frames>prior.capture+5));})&&!bad;
    }
    bool fft_projection() {
        auto* panel=qobject_cast<GraphPanel*>(fftDock->widget());
        auto* chart=panel?panel->active_chart():nullptr;
        if(chart&&wait([&]{return chart->value_count()>0&&chart->peak_value()>0;}))return true;
        std::fprintf(stderr,"selector FFT projection closed=%d hidden=%d exposed=%d count=%zu peak=%g mode=%s\n",int(fftDock->isClosed()),int(fftDock->isHidden()),int(fftDock->property("renderExposed").toBool()),chart?chart->value_count():0,chart?double(chart->peak_value()):0,panel?qPrintable(panel->display_mode()):"missing");
        return false;
    }
    bool selector_card(int choice) {
        auto* selectorCard=selectorDiagnostics.get();
        auto* selectorValues=selectorCard->findChild<QLabel*>("previewValues");
        auto* selectorFirst=selectorCard->findChild<QLabel*>("previewExactValue");
        phase=choice==1?23:33;
        emit inspectorScene->nodeSelected(200);
        if(wait([&]{return live_card(selectorCard,*selector->numeric_node());}))return true;
        std::fprintf(stderr,"selector card activity=%s values=%s first=%s lanes=%zu\n",qPrintable(selectorCard->property("previewActivity").toString()),qPrintable(selectorValues->text()),qPrintable(selectorFirst->text()),selector->numeric_node()->outputs.size());
        return false;
    }
    bool transition(int choice,int index,const auto& original_edges) {
        phase=choice==1?2:3;
        const SelectorTransition prior{fft_frames,generator_frames,capture_frames,revision,ordinal};
        transition_after=prior.revision;revision=0;
        if(!request_choice(choice,index)||!wait_choice(choice,prior))return false;
        phase=choice==1?21:31;
        if(selector->selected_input()!=choice||!selector->operation_summary().contains(selector->stream_name(unsigned(choice))))return false;
        subscribe();
        const auto switched=model->numeric_graph();
        if(!std::is_permutation(original_edges.begin(),original_edges.end(),switched.edges.begin(),switched.edges.end()))return false;
        if(!wait_transition_frames(choice,prior))return false;
        return transition_projection(choice);
    }
    bool transition_projection(int choice) {
        if(presenter.client().snapshot().statuses.value("analyzer").state!=State::Running)return false;
        if(!unavailable||choice==0){phase=choice==1?22:32;if(!fft_projection()||!selector_card(choice))return false;}
        return true;
    }
    bool transitions() {
        const auto original_edges=model->numeric_graph().edges;
        const auto source_before=generator_frames;
        int index=0;
        for(int choice:{1,0,1,0}) {
            if(!transition(choice,index,original_edges))return false;
            ++index;
        }
        return generator_frames>source_before+10;
    }
    bool capture_device_form(AudioNodeModel* capture,QWidget* captureCard) {
        model->set_capture_device_provider([](int,QString*){return capture_devices();});
        std::unique_ptr<QWidget> captureForm(capture->parameter_widget());
        auto* device=captureForm->findChild<QComboBox*>("captureDevice");
        if(!device)return false;
        const auto index=device->findData("fixture-loopback");
        if(index<0)return false;
        const auto prior_revision=revision,prior_ordinal=ordinal;
        transition_after=prior_revision;revision=0;
        device->setCurrentIndex(index);
        phase=6;
        return capture_recovery(capture,captureCard,prior_revision,prior_ordinal);
    }
    static QJsonArray capture_devices() {
        return QJsonArray{QJsonObject{{"id","fixture-loopback"},{"name","Silent fixture loopback"},{"is_default",false}}};
    }
    bool capture_recovery(AudioNodeModel* capture,QWidget* captureCard,std::uint64_t prior_revision,
                          std::uint64_t prior_ordinal) {
        if(!wait([&]{return presenter.client().snapshot().graph_revision>prior_revision&&!presenter.graph_request_pending();}))return false;
        subscribe();
        if(!wait([&]{return live_card(captureCard,*capture->numeric_node())&&ordinal>prior_ordinal;}))return false;
        std::puts("captureDevice automatic live Apply/current values/History recovery PASS");
        return true;
    }
    bool capture_device() {
        if(unavailable)return true;
        phase=4;
        emit inspectorScene->nodeSelected(13);
        auto* capture=model->delegateModel<AudioNodeModel>(13);
        std::unique_ptr<QWidget> captureDiagnostics(capture?capture->preview_widget():nullptr);
        auto* captureCard=captureDiagnostics.get();
        auto* captureValues=captureCard?captureCard->findChild<QLabel*>("previewValues"):nullptr;
        auto* captureFirst=captureCard?captureCard->findChild<QLabel*>("previewExactValue"):nullptr;
        return captureCard&&captureValues&&captureFirst&&capture_device_form(capture,captureCard);
    }
    bool grow_inputs() {
        auto expanded=*selector->numeric_node();
        if(!numeric_editor::set_input_count(expanded,3))return false;
        phase=71;
        if(!model->edit_node(200,expanded))return false;
        phase=72;
        model->addConnection({1,0,200,4});
        model->addConnection({1,source->port_index(QtNodes::PortType::Out,domain::sample_rate_output),200,5});
        QCoreApplication::processEvents();
        const auto configInputs=unsigned(selector->configuration_port(QtNodes::PortType::In,domain::ConfigurationRole::SampleClock)!=QtNodes::InvalidPortIndex)+unsigned(selector->configuration_port(QtNodes::PortType::In,domain::ConfigurationRole::FrequencyGrid)!=QtNodes::InvalidPortIndex);
        if(selector->numeric_node()->inputs.size()==6&&selector->nPorts(QtNodes::PortType::In)==6+configInputs&&model->connectionExists({1,0,200,4})&&model->connectionExists({1,source->port_index(QtNodes::PortType::Out,domain::sample_rate_output),200,5}))return true;
        std::fprintf(stderr,"selector growth inputs=%zu ports=%u config=%u edge6=%d edge7=%d possible6=%d possible7=%d\n",selector->numeric_node()->inputs.size(),selector->nPorts(QtNodes::PortType::In),configInputs,int(model->connectionExists({1,0,200,6})),int(model->connectionExists({1,source->port_index(QtNodes::PortType::Out,domain::sample_rate_output),200,7})),int(model->connectionPossible({1,0,200,6})),int(model->connectionPossible({1,source->port_index(QtNodes::PortType::Out,domain::sample_rate_output),200,7})));
        return false;
    }
    bool grow_selector() {
        phase=7;
        const auto prior_revision=revision,prior_ordinal=ordinal;
        transition_after=prior_revision;revision=0;
        if(!grow_inputs())return false;
        phase=73;
        if(!click_input(2))return false;
        selected=0;selected_frames=0;
        return grown_selection(prior_revision,prior_ordinal);
    }
    bool grown_selection(std::uint64_t prior_revision,std::uint64_t prior_ordinal) {
        phase=74;
        if(!wait([&]{return presenter.client().snapshot().graph_revision>prior_revision&&!presenter.graph_request_pending()&&applied_choice(2);}))return false;
        phase=75;subscribe();
        return wait([&]{return bad||(selected_frames>=5&&ordinal>prior_ordinal);})&&!bad;
    }
    bool resolved_zero(std::uint64_t before_pending) const {
        const auto active=presenter.applied_numeric_graph();
        if(!active||presenter.graph_request_pending()||presenter.client().snapshot().graph_revision<=before_pending)return false;
        for(const auto& node:active->nodes)if(node.id==200) {
            domain::ResolvedProcessingNode resolved;
            domain::ProcessingParameterView value;
            return bool(domain::resolve_processing_node(node,numeric_editor::limits(),resolved))&&
                   domain::resolve_processing_parameter(resolved,"selected_input",value)==Status::ok&&
                   std::visit([](auto x){return x==0;},std::get<NumericLiteral>(value));
        }
        return false;
    }
    bool pending_edit() {
        const auto before_pending=presenter.client().snapshot().graph_revision;
        phase=76;presenter.apply_pipeline_graph({});selector->select_input(0);
        if(!wait([&]{return resolved_zero(before_pending);}))return false;
        phase=77;subscribe();
        return true;
    }
    bool open_surface() {
        auto* surfaceDock=primary_dock(8);
        if(!surfaceDock)return false;
        surfaceDock->toggleView(true);surfaceDock->raise();
        surface=surfaceDock->findChild<SpectrogramGraph*>();
        if(!surface)return false;
        surfacePanel=qobject_cast<GraphPanel*>(surfaceDock->widget());
        std::fprintf(stderr,"selector surface mode=%s active=%d hidden=%d exposed=%d frames=%zu bands=%zu revision=%llu endpoint=%d metrics=%d paused=%d status=%s\n",
            surfacePanel?qPrintable(surfacePanel->display_mode()):"none",int(surfacePanel&&surfacePanel->active_spectrogram()==surface),int(surfaceDock->isHidden()),int(surfaceDock->property("renderExposed").toBool()),
            surface->projected_history_frames(),surface->projected_history_bands(),static_cast<unsigned long long>(surface->history_projection_revision()),int(surfaceDock->property("numericEndpoint").isValid()),int(surfacePanel&&surfacePanel->render_target(true).metrics),int(surface->paused()),qPrintable(surface->projected_status()));
        return surfacePanel&&surfacePanel->active_spectrogram()==surface&&wait([&]{return !surfacePanel->value_layout().isEmpty();});
    }
    std::optional<std::size_t> surface_bands() const {
        const auto graph=presenter.applied_numeric_graph();
        if(!graph)return {};
        const auto row=std::find_if(graph->nodes.begin(),graph->nodes.end(),[](const auto& node){return node.kind=="ArrayConcat";});
        if(row==graph->nodes.end())return {};
        return std::get<NumericContract>(row->outputs[0].contract.view()).shape.extents[0].maximum;
    }
    std::optional<float> surface_peak(std::size_t bands) const {
        float peak=0;
        for(std::size_t frame=0;frame<surface->projected_history_frames();++frame)for(std::size_t band=0;band<bands;++band) {
            float value=0;
            if(!surface->projected_history_sample(frame,band,value)||!std::isfinite(value))return {};
            peak=std::max(peak,value);
        }
        return peak;
    }
    bool surface_projection() {
        if(!open_surface())return false;
        const auto bands=surface_bands();
        if(!bands)return false;
        phase=78;
        if(!wait([&]{return surface->projected_history_frames()>0&&surface->projected_history_bands()==*bands&&surface->history_projection_revision()>0;}))return false;
        const auto peak=surface_peak(*bands);
        if(!peak)return false;
        return finish_surface(*peak);
    }
    bool finish_surface(float peak) {
        phase=79;
        if(!(peak>0))return false;
        phase=80;
        const auto projected=surface->history_projection_revision();
        if(!wait([&]{return surface->history_projection_revision()>projected;}))return false;
        std::printf("app-owned History to renderer projection frames=%zu bands=%zu peak=%.9g (pre-GL)\n",surface->projected_history_frames(),surface->projected_history_bands(),peak);
        std::printf("startup continuous N selector/live undo redo/injected unavailable=%d History ordinal=%llu sequence=%llu PASS\n",int(unavailable),static_cast<unsigned long long>(ordinal),static_cast<unsigned long long>(history_sequence));
        return true;
    }
    WorkspaceDock* primary_dock(int node) const {
        const auto docks=window->workspaceDocks();
        for(auto* dock:docks)
            if(dock->property("graphNode").isValid()&&dock->property("graphNode").toInt()==node&&dock->property("viewInstance").toInt()==0)return dock;
        return nullptr;
    }
    bool open_fft() {
        fftDock=primary_dock(10);
        if(!fftDock)return false;
        // Explicitly expose this renderer; raising a closed ADS dock does not render it.
        fftDock->toggleView(true);fftDock->raise();
        diagnostic.emplace([&]{print_diagnostic();});
        return true;
    }
    bool initial_subscriptions() {
        connect_observers();
        if(!wait([&]{return !presenter.graph_request_pending()&&presenter.client().snapshot().statuses.value("analyzer").state==State::Running;}))return false;
        subscribe();phase=1;
        return wait([&]{return bad||initial_frames();})&&!bad;
    }
public:
    StartupSelectorProbe(CoreApplication& app,MainWindow* window,bool unavailable)
        :app(app),window(window),unavailable(unavailable),presenter(app.presenter()){}
    bool run() {
        if(!source_card()||!editor_fixture())return false;
        transition_after=presenter.client().snapshot().graph_revision;
        if(!open_fft()||!initial_subscriptions())return false;
        if(!transitions()||!capture_device()||!grow_selector()||!pending_edit()||!surface_projection())return false;
        return presenter.client().snapshot().statuses.value("analyzer").state==State::Running&&!bad;
    }
};
} // namespace shipped_gui_probe
inline bool verify_startup_selector(CoreApplication& app,MainWindow* window,bool unavailable) {
    return shipped_gui_probe::StartupSelectorProbe(app,window,unavailable).run();
}

namespace shipped_gui_probe {
class ExampleAutosaveProbe {
    CoreApplication& app;
    MainWindow* window;
    const QString& path;
    GuiPresenter& presenter;
    NumericEditorGraphModel* model=nullptr;
    QCheckBox* autosave=nullptr;
    QLabel* validation=nullptr;
    std::unique_ptr<QWidget> form;
    desktop::NumericProjectDocument saved;
    domain::NumericProcessingGraph disk;
    std::optional<domain::NumericProcessingGraph> applied;
    std::uint64_t working_revision=0;

    static QByteArray encoded(auto graph) {
        std::sort(graph.edges.begin(),graph.edges.end(),[](const auto& a,const auto& b){return std::tie(a.from,a.to,a.output_port,a.input_port)<std::tie(b.from,b.to,b.output_port,b.input_port);});
        QByteArray bytes;
        desktop::encode_numeric_graph(graph,bytes);
        return bytes;
    }
    bool failure(const char* phase) const {
        const auto snapshot=presenter.client().snapshot();
        std::fprintf(stderr,"Autosave phase failed: %s revision=%llu working=%llu pending=%d modified=%d model=%d autosave=%d validation=%d diagnostic=%s\n",phase,
            (unsigned long long)snapshot.graph_revision,(unsigned long long)working_revision,int(presenter.graph_request_pending()),int(app._ctx.editor_draft_modified),int(model!=nullptr),int(autosave!=nullptr),int(validation!=nullptr),validation?qPrintable(validation->toolTip()):"missing");
        return false;
    }
    bool fixture() {
        model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
        autosave=window->findChild<QCheckBox*>("enableAutosave");
        validation=window->findChild<QLabel*>("pipelineValidation");
        return model&&autosave&&validation;
    }
    bool edit_amplitude() {
        autosave->setChecked(false);
        const auto revision=presenter.client().snapshot().graph_revision;
        auto* source=model->delegateModel<AudioNodeModel>(1);
        if(!source)return false;
        form.reset(source->parameter_widget());
        auto* amplitude=form->findChild<QLineEdit*>("oscillator0amplitude");
        if(!amplitude)return false;
        amplitude->setText("0.2");amplitude->setModified(true);
        QMetaObject::invokeMethod(amplitude,"editingFinished",Qt::DirectConnection);
        if(wait([&]{return presenter.client().snapshot().graph_revision>revision&&!presenter.graph_request_pending();},5000)&&!QFile::exists(path))return true;
        qWarning()<<"Autosave probe: live edit or disabled persistence";
        return false;
    }
    bool preference_saved(const QString& key,const QJsonValue& expected) {
        QJsonObject settings;
        return desktop::load_numeric_project(path,saved,disk)&&desktop::read_numeric_application_settings(saved,settings)&&
               settings["ui"].toObject()[key]==expected;
    }
    bool flush_disabled_presentation() {
        auto* editor=window->data_flow_editor();
        if(!editor || autosave->isChecked())return false;
        const auto revision=presenter.client().snapshot().graph_revision;
        // Consume the editor's deferred presentation save while persistence is
        // still disabled, so enabling autosave tests only the preference write.
        editor->flush_presentation();
        return app._ctx.editor_draft_modified && !QFile::exists(path) &&
               presenter.client().snapshot().graph_revision==revision && !presenter.graph_request_pending();
    }
    bool preferences() {
        if(!flush_disabled_presentation())return failure("preferences.flush_disabled");
        // A forced preferences-only request preserves the dirty document.
        // Enabling autosave also flushes pending project changes asynchronously.
        emit window->settings_change_requested({ui::SettingsChangeScope::ApplicationPreferences,true});
        if(!wait([&]{return preference_saved(QStringLiteral("autosave_enabled"),false);},5000)) {
            qWarning()<<"Autosave probe: preferences save"<<window->statusBar()->currentMessage()<<QFile::exists(path);
            return false;
        }
        if(encoded(disk)==encoded(numeric_startup_graph())&&desktop::read_numeric_applied(saved,applied)&&!applied&&app._ctx.editor_draft_modified)return true;
        qWarning()<<"Autosave probe: snapshot boundary"<<disk.nodes.size()<<bool(applied)<<app._ctx.editor_draft_modified;
        return false;
    }
    bool snapshots_saved() {
        return desktop::load_numeric_project(path,saved,disk)&&desktop::read_numeric_applied(saved,applied)&&applied&&
               encoded(disk)==encoded(model->numeric_graph())&&encoded(*applied)==encoded(*presenter.applied_numeric_graph());
    }
    bool full_save() {
        // This scenario is executed offscreen; these calls must never activate a native desktop window.
        window->activateWindow();window->setFocus();
        auto* save=window->findChild<QAction*>("saveSettings");
        if(!wait([&]{return save&&save->isEnabled();},5000)){qWarning()<<"Autosave probe: Save context";return false;}
        save->trigger();
        if(!wait([&]{return snapshots_saved();},5000)){qWarning()<<"Autosave probe: full Save snapshots";return false;}
        autosave->setChecked(true);
        return wait([&]{return preference_saved(QStringLiteral("autosave_enabled"),true);},5000);
    }
    bool save_position() {
        working_revision=presenter.client().snapshot().graph_revision;
        model->setNodeData(1,QtNodes::NodeRole::Position,QPointF(713,421));
        if(!wait([&]{desktop::NumericDraftPresentation display;return desktop::load_numeric_project(path,saved,disk)&&
                   desktop::read_numeric_presentation(saved,display)&&display.positions["1"].toObject()["x"].toDouble()==713;},5000)) {
            desktop::NumericDraftPresentation display;
            const auto loaded=desktop::load_numeric_project(path,saved,disk)&&desktop::read_numeric_presentation(saved,display);
            auto* status=window->findChild<QLabel*>("configurationSaveStatus");
            qWarning()<<"Autosave probe: position save"<<loaded<<display.positions.value("1")
                      <<model->nodeData(1,QtNodes::NodeRole::Position)<<app._ctx.file_configs.ui.autosave_enabled
                      <<(status?status->property("saveState"):QVariant{})<<(status?status->toolTip():QString{});
            return false;
        }
        return presenter.client().snapshot().graph_revision==working_revision;
    }
    bool reject_draft(AudioNodeModel* node,const auto& good) {
        auto invalid=good;
        std::get<domain::TransformProcessingParameters>(invalid.parameters).values={{"size",signal_contracts::NumericLiteral{std::uint64_t{63}}}};
        node->set_numeric_node(invalid);
        // Invalid edits remain in the editable draft and are autosaved, while the
        // last successfully applied graph must remain untouched and running.
        if(!wait([&]{return node->numeric_node() &&
                   !model->node_error(*node->numeric_node()).isEmpty() &&
                   !validation->toolTip().isEmpty() && snapshots_saved();},5000))return false;
        return presenter.client().snapshot().graph_revision==working_revision&&
               presenter.client().snapshot().statuses.value("analyzer").state==State::Running;
    }
    bool theme_preserves_snapshots(const QByteArray& savedDraft,const QByteArray& savedApplied) {
        auto* theme=window->findChild<QComboBox*>("interfaceTheme");
        if(!theme)return false;
        theme->setCurrentIndex(1);
        if(!wait([&]{return preference_saved(QStringLiteral("theme"),QJsonValue(QStringLiteral("dark")));},5000)) {
            qWarning()<<"Autosave rejected draft: theme preference not saved";return false;
        }
        const bool same_draft=encoded(disk)==savedDraft;
        const bool applied_read=bool(desktop::read_numeric_applied(saved,applied));
        const bool same_applied=applied_read&&applied&&encoded(*applied)==savedApplied;
        if(!same_draft || !same_applied)
            qWarning()<<"Autosave rejected draft: same draft"<<same_draft
                      <<"matches model"<<(encoded(disk)==encoded(model->numeric_graph()))
                      <<"applied readable"<<applied_read<<"same applied"<<same_applied;
        return same_draft&&same_applied;
    }
    bool rejected_draft() {
        auto* node=model->delegateModel<AudioNodeModel>(2);
        if(!node)return false;
        const auto good=*node->numeric_node();
        if(!reject_draft(node,good))return failure("rejected_draft.reject");
        const auto savedDraft=encoded(disk),savedApplied=encoded(*applied);
        if(!theme_preserves_snapshots(savedDraft,savedApplied))return failure("rejected_draft.theme");
        if(presenter.client().snapshot().graph_revision!=working_revision)return failure("rejected_draft.revision");
        node->set_numeric_node(good);
        std::puts("continuous live Apply independent of autosave; position-only save and rejected draft preserve runtime PASS");
        return true;
    }
public:
    ExampleAutosaveProbe(CoreApplication& app,MainWindow* window,const QString& path)
        :app(app),window(window),path(path),presenter(app.presenter()){}
    bool run() {
        if(!fixture())return failure("fixture");
        if(!edit_amplitude())return failure("edit_amplitude");
        if(!preferences())return failure("preferences");
        if(!full_save())return failure("full_save");
        if(!save_position())return failure("save_position");
        if(!rejected_draft())return failure("rejected_draft");
        return true;
    }
};
} // namespace shipped_gui_probe
inline bool verify_example_autosave(CoreApplication& app,MainWindow* window,const QString& path) {
    return shipped_gui_probe::ExampleAutosaveProbe(app,window,path).run();
}
