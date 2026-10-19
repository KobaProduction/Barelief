#pragma once
#include "synthetic_capture.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <desktop/numeric_project_document.h>
#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonDocument>
#include <QMenu>
#include <QAction>
#include <QLabel>
#include <QFile>
#include <QStatusBar>
#include <array>
#include <stdexcept>
#include <functional>
#include <algorithm>
#include <atomic>
#include <bit>
namespace gui_fixture {
inline void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
inline bool wait(const std::function<bool()>& condition,int duration=3000){QElapsedTimer clock;clock.start();while(clock.elapsed()<duration){QCoreApplication::processEvents();if(condition())return true;QThread::msleep(3);}return condition();}
inline QJsonObject encoded(const domain::NumericProcessingGraph& graph){QByteArray bytes;check(bool(desktop::encode_numeric_graph(graph,bytes)),"encode canonical graph");return QJsonDocument::fromJson(bytes).object();}
inline void save(const QString& path,const domain::NumericProcessingGraph& graph,QJsonObject ui={}){
    ui["autosave_enabled"]=false;ui["pipeline_auto_apply"]=false;
    desktop::NumericDraftPresentation settings;settings.application_settings={{"ui",ui}};
    int position=0;for(const auto& node:graph.nodes){settings.positions[QString::number(node.id)]=QJsonObject{{"x",(position%4)*10000},{"y",(position/4)*10000}};++position;}
    desktop::NumericProjectDocument source;check(bool(desktop::create_numeric_project(graph,settings,source)),"create canonical fixture");check(bool(desktop::save_numeric_project(source,path)),"save isolated fixture");
}
struct App {
    QTemporaryDir directory;
    domain::NumericProcessingGraph graph;
    QByteArray path;char name[24]="numeric-gui-fixture",option[9]="--config";
    int argc=3;std::array<char*,4> argv{};
    std::unique_ptr<CoreApplication> application;
    std::shared_ptr<std::atomic_bool> live_delivery=std::make_shared<std::atomic_bool>(false);
    // Keep the producer alive while deterministic renderer checks inject frames.
    // Only the test adapter's delivery opens for the subsequent live phase.
    class GatedCapture final:public pipeline::IAudioSource {
        std::shared_ptr<std::atomic_bool> gate;
        test_fixture::SyntheticCapture producer;
    public:
        GatedCapture(pipeline::AudioSourceKind kind,std::shared_ptr<std::atomic_bool> value):gate(std::move(value)),producer(kind,true){}
        void start() override {producer.start();}
        void stop() override {producer.stop();}
        bool is_running() const override {return producer.is_running();}
        pipeline::AudioSourceKind source() const override {return producer.source();}
        void configure(const QJsonObject& settings) override {producer.configure(settings);}
        bool reconfigure_live(const QJsonObject& settings) override {return producer.reconfigure_live(settings);}
        pipeline::AudioSourceDiagnostics diagnostics() const override {return producer.diagnostics();}
        void set_status_handler(StatusHandler handler) override {producer.set_status_handler(std::move(handler));}
        void set_lifecycle_handler(LifecycleHandler handler) override {producer.set_lifecycle_handler(std::move(handler));}
        void set_audio_handler(AudioHandler handler) override {
            producer.set_audio_handler([enabled=gate,handler=std::move(handler)](auto frame){if(enabled->load(std::memory_order_acquire)&&handler)handler(std::move(frame));});
        }
    };
    MainWindow* window=nullptr;SchemeEditorWidget* editor=nullptr;
    void configure_test_sources() {
        for(auto& node:graph.nodes)if(node.kind=="TestSignal"){
            node.kind="AudioSource";node.parameters=domain::EndpointProcessingParameters{};
        }
    }
    void write_project(const QJsonObject& ui,const QByteArray& stored) {
        check(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen","only isolated offscreen fixture is supported");
        check(directory.isValid(),"temporary fixture directory");
        path=directory.filePath("project.json").toLocal8Bit();
        if(stored.isEmpty())save(QString::fromLocal8Bit(path),graph,ui);
        else write_restart_source(stored);
    }
    void write_restart_source(const QByteArray& stored) const {
        QFile file(QString::fromLocal8Bit(path));
        check(file.open(QIODevice::WriteOnly) && file.write(stored)==stored.size(),"write isolated restart source");
    }
    pipeline::AudioSourceFactory capture_factory(bool deliver_audio) const {
        return [deliver_audio,gate=live_delivery](auto kind)->std::unique_ptr<pipeline::IAudioSource>{
            if(deliver_audio)return std::make_unique<GatedCapture>(kind,gate);
            return std::make_unique<test_fixture::SyntheticCapture>(kind,false);
        };
    }
    void configure_discovery() {
        // Discovery and capture describe the same injected device, not the host.
        auto* numeric=dynamic_cast<NumericEditorGraphModel*>(application->_ctx.data_flow_graph.get());
        check(numeric,"numeric fixture model");
        numeric->set_capture_device_provider([](int,QString*) {
            return QJsonArray{QJsonObject{{"id","synthetic-capture"},{"name","Synthetic capture"},{"is_default",true},
                {"native_sample_rate",48000},{"native_channels",1}}};
        });
        application->_ctx.file_configs.ui.autosave_enabled=false;
    }
    void show_window() {
        for(auto* widget:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(widget))window=found;
        check(window,"MainWindow fixture");
        editor=window->findChild<SchemeEditorWidget*>();
        check(editor,"editor fixture");
        window->show();
    }
    void diagnose_startup(bool restart) {
        const auto snapshot=presenter().client().snapshot();
        qWarning()<<"GUI fixture startup diagnostic"<<"restart"<<restart<<"revision"<<snapshot.graph_revision
            <<"pending"<<presenter().graph_request_pending()<<"apply"<<presenter().apply_status()
            <<"saved"<<presenter().saved_graph_warning()<<"file"<<presenter().file_protection_warning()
            <<"status"<<window->statusBar()->currentMessage();
    }
    void await_startup(bool restart) {
        const bool started=wait([&]{return presenter().client().snapshot().graph_revision>0 && !presenter().graph_request_pending();});
        if(!started)diagnose_startup(restart);
        check(started,"initial continuous graph");
    }
    explicit App(domain::NumericProcessingGraph configured=numeric_default_graph(),QJsonObject ui={},QByteArray stored={},bool deliver_audio=false) :graph(std::move(configured)) {
        configure_test_sources();
        write_project(ui,stored);
        argv={name,option,path.data(),nullptr};
        application=std::make_unique<CoreApplication>(argc,argv.data(),capture_factory(deliver_audio));
        configure_discovery();
        show_window();
        await_startup(!stored.isEmpty());
    }
    GuiPresenter& presenter(){return application->presenter();}
    void apply(){const auto before=presenter().client().snapshot().graph_revision;presenter().apply_pipeline_graph(editor->document());check(wait([&]{return presenter().client().snapshot().graph_revision>before && presenter().applied_numeric_graph().has_value();}),"explicit Apply");}
    WorkspaceDock* dock(const QString& name){auto* value=window->workspaceDock(name);check(value,"named dock missing");return value;}
    std::unique_ptr<QMenu> graph_menu(int node) const {
        auto* scene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();
        check(scene && scene->nodeGraphicsObject(node),"scene node for action");
        auto* item=scene->nodeGraphicsObject(node);
        return std::unique_ptr<QMenu>(scene->createSceneMenu(item->sceneBoundingRect().center()));
    }
    static QAction* graph_action(QMenu& menu,int node,int placement,std::optional<unsigned> port) {
        const auto name=port?QStringLiteral("openGraphPort%1").arg(*port):QStringLiteral("openGraphPlacement%1").arg(placement);
        auto* action=menu.findChild<QAction*>(name);
        if(!action){
            qWarning()<<"Graph menu diagnostic"<<node<<placement<<menu.actions();
            for(auto* candidate:menu.findChildren<QAction*>())qWarning()<<candidate->objectName()<<candidate->text();
        }
        check(action,"graph placement/port QAction");
        return action;
    }
    static QList<WorkspaceDock*> current_docks() {
        QList<WorkspaceDock*> docks;
        for(auto* widget:QApplication::allWidgets())if(auto* dock=qobject_cast<WorkspaceDock*>(widget))docks.append(dock);
        return docks;
    }
    static WorkspaceDock* new_dock(int node,const QList<WorkspaceDock*>& before) {
        for(auto* dock:current_docks())
            if(!before.contains(dock) && dock->property("graphNode").toInt()==node)return dock;
        return nullptr;
    }
    static WorkspaceDock* await_new_dock(int node,const QList<WorkspaceDock*>& before) {
        WorkspaceDock* created=nullptr;
        check(wait([&]{created=new_dock(node,before);return created!=nullptr;}),"actual graph action creates new instance");
        return created;
    }
    static void set_created_placement(WorkspaceDock& created,int placement) {
        auto* action=created.findChild<QAction*>("graphPlacement");
        check(action && action->menu() && action->menu()->actions().size()>placement,"created dock placement menu");
        action->menu()->actions()[placement]->trigger();
        QCoreApplication::processEvents();
    }
    WorkspaceDock* open(int node,int placement,std::optional<unsigned> port={}){
        auto menu=graph_menu(node);
        auto* action=graph_action(*menu,node,placement,port);
        const auto before=current_docks();
        action->trigger();
        auto* created=await_new_dock(node,before);
        if(port && placement!=1)set_created_placement(*created,placement);
        return created;
    }
    ~App(){application.reset();}
};
constexpr signal_contracts::FrameLimits frame_limits{8,32768,262144,16384,256,8192};
struct Arena {
    alignas(64) std::array<std::byte,1048576> payload{};
    alignas(64) std::array<std::byte,131072> metadata{};
    std::array<signal_contracts::BufferSlot,256> payload_slots{},metadata_slots{};
    signal_contracts::BufferPool p{payload,std::span(payload_slots).first(64),16384},m{metadata,metadata_slots,512};
};
inline std::shared_ptr<pipeline::NumericDeliveryContext> context(const domain::NumericProcessingGraph& graph,int node,domain::PortDirection direction,unsigned port=0){
    auto result=std::make_shared<pipeline::NumericDeliveryContext>();result->endpoint={graph.graph_id,node,direction,std::uint16_t(port)};
    const auto n=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& v){return v.id==node;});check(n!=graph.nodes.end(),"context node");const auto& ports=direction==domain::PortDirection::Input?n->inputs:n->outputs;
    const auto selected=std::find_if(ports.begin(),ports.end(),[&](const auto& v){return v.id==port;});check(selected!=ports.end(),"context port");result->contract=selected->contract;result->retention_owner=std::make_shared<Arena>();return result;
}
inline std::vector<std::byte> explicit_fixture_coordinates(const signal_contracts::AxisCoordinates& axis,std::size_t count) {
    if(axis.mode!=signal_contracts::CoordinateMode::ExplicitFloat64LE)return {};
    std::vector<std::byte> bytes(count*8);
    for(std::size_t index=0;index<count;++index){const auto bits=std::bit_cast<std::uint64_t>(axis.unit==signal_contracts::NumericUnit::Hertz?double(index+1)*100.:double(index));for(unsigned byte=0;byte<8;++byte)bytes[index*8+byte]=std::byte((bits>>(byte*8))&255);}
    return bytes;
}
inline signal_contracts::FrameStamp fixture_stamp(std::uint64_t sequence) {
    signal_contracts::FrameStamp stamp;
    stamp.sequence=sequence;stamp.producer_epoch=711;
    return stamp;
}
inline signal_contracts::FrameStamp history_stamp(std::uint64_t ordinal,bool full=false) {
    auto stamp=fixture_stamp(ordinal);
    stamp.timestamp_ticks=std::int64_t(ordinal*4096);
    stamp.discontinuity=full?ordinal<=1:ordinal==1;
    if(full)stamp.generation=ordinal==0?1:0;
    return stamp;
}
inline std::vector<signal_contracts::PlaneLayout> float_planes(std::size_t planes,std::size_t values,std::size_t columns=1) {
    std::vector<signal_contracts::PlaneLayout> result;
    for(std::size_t plane=0;plane<planes;++plane)
        result.push_back({plane*values*sizeof(float),{columns*sizeof(float),sizeof(float)}});
    return result;
}
inline pipeline::DeliveredNumericFrame retain_fixture_vector(const std::shared_ptr<pipeline::NumericDeliveryContext>& ctx,
    pipeline::DeliveryStamp delivery,const signal_contracts::DataFrameView& view,bool compact) {
    pipeline::DeliveredNumericFrame result;
    const auto arena=std::static_pointer_cast<const Arena>(ctx->retention_owner);
    auto* writable=const_cast<Arena*>(arena.get());
    check(pipeline::retain_numeric_delivery(ctx,delivery,view,signal_contracts::Status::ok,frame_limits,
        writable->p,writable->m,result,compact)==signal_contracts::Status::ok,compact?"own History delivery":"own float delivery");
    return result;
}
inline pipeline::DeliveredNumericFrame retain_float_vector(const std::shared_ptr<pipeline::NumericDeliveryContext>& ctx,
    pipeline::DeliveryStamp delivery,const signal_contracts::NumericContract& contract,
    const signal_contracts::FrameStamp& stamp,float value,bool compact) {
    const auto count=contract.shape.extents[0].minimum;
    const std::vector<float> data(count*contract.channel_order.size(),value);
    const auto planes=float_planes(contract.channel_order.size(),count);
    const auto columns=explicit_fixture_coordinates(contract.coordinates[0],count);
    const signal_contracts::NumericView view{&contract,std::as_bytes(std::span(data)),planes,{count,1},{},{{columns,{}}}};
    return retain_fixture_vector(ctx,delivery,{stamp,view},compact);
}
inline pipeline::DeliveredNumericFrame floats(const std::shared_ptr<pipeline::NumericDeliveryContext>& ctx,pipeline::DeliveryStamp delivery,std::uint64_t sequence,float value){
    using namespace signal_contracts;
    const auto contract=std::get<NumericContract>(ctx->contract.view());
    check(contract.dtype==DType::Float32 && contract.shape.rank==Rank::Vector,"Float32 vector fixture");
    return retain_float_vector(ctx,delivery,contract,fixture_stamp(sequence),value,false);
}
inline pipeline::DeliveredNumericFrame scalar(const std::shared_ptr<pipeline::NumericDeliveryContext>& ctx,pipeline::DeliveryStamp delivery,std::uint64_t sequence,std::uint64_t value){
    using namespace signal_contracts;auto contract=std::get<NumericContract>(ctx->contract.view());check(contract.dtype==DType::UInt64 && contract.shape.rank==Rank::Scalar,"UInt64 scalar fixture");
    const std::array<PlaneLayout,1> planes{{{0,{8,8}}}};NumericView view{&contract,std::as_bytes(std::span(&value,1)),planes,{1,1}};
    FrameStamp stamp;stamp.sequence=sequence;stamp.producer_epoch=711;pipeline::DeliveredNumericFrame result;
    auto arena=std::static_pointer_cast<const Arena>(ctx->retention_owner);auto* writable=const_cast<Arena*>(arena.get());
    check(pipeline::retain_numeric_delivery(ctx,delivery,{stamp,view},Status::ok,frame_limits,writable->p,writable->m,result)==Status::ok,"own scalar delivery");return result;
}
inline signal_contracts::NumericContract compact_history_contract(const signal_contracts::NumericContract& contract) {
    using namespace signal_contracts;
    auto row=contract;
    row.semantic=contract.history_element;
    row.history_element=Semantic::Numeric;
    row.coordinates={contract.coordinates[1],AxisCoordinates{}};
    row.shape={Rank::Vector,{contract.shape.axes[1],Axis::None},{contract.shape.extents[1],ExtentBounds{}}};
    return row;
}
inline pipeline::DeliveredNumericHistoryRow history_event(const signal_contracts::NumericContract& contract,std::uint64_t ordinal) {
    pipeline::DeliveredNumericHistoryRow result;
    result.event=pipeline::NumericHistoryEvent::Row;
    result.capacity=contract.shape.extents[0].maximum;
    result.filled_count=std::min(ordinal,std::uint64_t(result.capacity));
    result.accepted_row_ordinal=ordinal;
    return result;
}
inline pipeline::DeliveredNumericHistoryRow history(const std::shared_ptr<pipeline::NumericDeliveryContext>& ctx,pipeline::DeliveryStamp delivery,std::uint64_t ordinal,float value){
    using namespace signal_contracts;
    const auto contract=std::get<NumericContract>(ctx->contract.view());
    check(contract.dtype==DType::Float32 && contract.semantic==Semantic::History,"Float32 History fixture");
    auto result=history_event(contract,ordinal);
    result.compact=retain_float_vector(ctx,delivery,compact_history_contract(contract),history_stamp(ordinal),value,true);
    return result;
}
struct ColdArena {
    alignas(64) std::array<std::byte,1048576> payload{};
    alignas(64) std::array<std::byte,131072> metadata{};
    std::array<signal_contracts::BufferSlot,8> payload_slots{},metadata_slots{};
    signal_contracts::BufferPool p{payload,payload_slots,131072},m{metadata,metadata_slots,16384};
};
inline void fill_history_plane(std::span<float> data,std::size_t columns,std::size_t capacity,std::size_t filled,float latest) {
    for(std::size_t row=0;row<filled;++row)
        std::fill_n(data.begin()+(capacity-filled+row)*columns,columns,latest-float(filled-1-row));
}
inline std::vector<float> full_history_values(const signal_contracts::NumericContract& contract,std::uint64_t ordinal,float latest) {
    const auto capacity=contract.shape.extents[0].maximum,columns=contract.shape.extents[1].minimum;
    const auto filled=std::min(ordinal,capacity);
    std::vector<float> data(capacity*columns*contract.channel_order.size());
    for(std::size_t plane=0;plane<contract.channel_order.size();++plane)
        fill_history_plane(std::span(data).subspan(plane*capacity*columns,capacity*columns),columns,capacity,filled,latest);
    return data;
}
inline void encode_time_coordinate(std::span<std::byte> target,double seconds) {
    const auto bits=std::bit_cast<std::uint64_t>(seconds);
    for(unsigned byte=0;byte<8;++byte)target[byte]=std::byte((bits>>(byte*8))&255);
}
inline std::vector<std::byte> full_history_times(const signal_contracts::NumericContract& contract,std::uint64_t ordinal) {
    using signal_contracts::CoordinateMode;
    const auto capacity=contract.shape.extents[0].maximum;
    const auto filled=std::min(ordinal,capacity);
    std::vector<std::byte> times(contract.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE?capacity*8:0);
    for(std::size_t row=0;row<filled && !times.empty();++row) {
        const double seconds=double(ordinal-filled+row+1)*4096*contract.clock.tick_seconds.numerator/contract.clock.tick_seconds.denominator;
        encode_time_coordinate(std::span(times).subspan((capacity-filled+row)*8,8),seconds);
    }
    return times;
}
inline pipeline::DeliveredNumericFrame retain_fixture_matrix(const std::shared_ptr<pipeline::NumericDeliveryContext>& ctx,
    pipeline::DeliveryStamp delivery,const signal_contracts::DataFrameView& frame) {
    using namespace signal_contracts;
    auto arena=std::make_shared<ColdArena>();
    auto retainedContext=std::make_shared<pipeline::NumericDeliveryContext>(*ctx);
    retainedContext->retention_owner=arena;
    pipeline::DeliveredNumericFrame result{retainedContext,delivery,{},Status::ok};
    check(retain_frame(frame,frame_limits,arena->p,arena->m,result.frame)==Status::ok,"own full matrix snapshot");
    return result;
}
inline pipeline::DeliveredNumericFrame full_history(const std::shared_ptr<pipeline::NumericDeliveryContext>& ctx,
    pipeline::DeliveryStamp delivery,std::uint64_t ordinal,float latest) {
    using namespace signal_contracts;
    const auto contract=std::get<NumericContract>(ctx->contract.view());
    check(contract.shape.rank==Rank::Matrix && contract.dtype==DType::Float32,"full Float32 matrix fixture");
    const auto capacity=contract.shape.extents[0].maximum,columns=contract.shape.extents[1].minimum;
    const auto data=full_history_values(contract,ordinal,latest);
    const auto planes=float_planes(contract.channel_order.size(),capacity*columns,columns);
    const auto times=full_history_times(contract,ordinal);
    const auto frequencies=explicit_fixture_coordinates(contract.coordinates[1],columns);
    const NumericView numeric{&contract,std::as_bytes(std::span(data)),planes,{capacity,columns},{capacity,0},{{times,frequencies}}};
    return retain_fixture_matrix(ctx,delivery,{history_stamp(ordinal,true),numeric});
}
} // namespace gui_fixture
