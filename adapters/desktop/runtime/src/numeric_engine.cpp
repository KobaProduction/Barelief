#include "numeric_engine_ingress.h"
#include <pipeline/numeric_engine.h>
#include <desktop/numeric_project_document.h>
#include <control/numeric_remote_support.h>
#include <dataflow_runtime/runtime.h>
#include <QJsonDocument>
#include <QDebug>
#include <QJsonArray>
#include <QTimer>
#include <core/performance.h>
#include <QMetaObject>
#include <QUdpSocket>
#include <wled/numeric_wled_output_policy.h>
#include <bdsp_protocol/numeric_data_codec.h>
#include <chrono>
#include <array>
#include <map>
#include <mutex>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <QPointer>
#include <limits>
#include <cmath>

namespace pipeline {
namespace {
using namespace signal_contracts;
using namespace numeric_engine_detail;
dataflow_runtime::NumericRuntimeProfile desktop_profile() {
    // Process admission charges bounded delivery and control ownership ceilings
    // separately from the graph's plan/payload/scratch. These are reservations,
    // not an assertion that each ceiling is allocated at startup.
    constexpr std::size_t fixed=256*(262144+16384)+(384+48)*1024*1024;
    auto profile=dataflow_runtime::NumericRuntimeProfile{{512,2048,8,32,256,4*1024*1024,frame_limits},
        {{16*1024*1024,32*1024*1024,16*1024*1024,1024*1024},fixed,576*1024*1024},
        {32768,8,262144,262144,16*1024*1024},{true,true,true},true};
    profile.monotonic_nanoseconds=[](void*) noexcept -> std::uint64_t {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    return profile;
}

}

bool resolve_native_capture_graph(domain::NumericProcessingGraph& graph,
    const std::map<domain::NodeId,QString>& devices,QString* error) {
    if(error)error->clear();
    auto candidate=graph;
    for(const auto& source:graph.nodes)if(source.kind=="AudioSource") {
        AudioSourceKind kind=AudioSourceKind::SystemOutput;
        if(const auto* parameters=std::get_if<domain::EndpointProcessingParameters>(&source.parameters))
            for(const auto& parameter:parameters->values)if(parameter.key=="source")
                if(const auto* literal=std::get_if<NumericLiteral>(&parameter.value))kind=AudioSourceKind(std::visit([](auto v){return int(v);},*literal));
        if(kind!=AudioSourceKind::SystemOutput && kind!=AudioSourceKind::Microphone) {
            if(error)*error=QStringLiteral("Invalid physical source kind");return false;
        }
        const auto endpoints=enumerate_audio_capture_devices(kind);
        const auto configured=devices.find(source.id);
        const auto endpoint=std::find_if(endpoints.begin(),endpoints.end(),[&](const auto& device){
            return configured==devices.end()?device.is_default:device.id==configured->second;
        });
        if(endpoint!=endpoints.end() && endpoint->native_sample_rate && endpoint->native_channels)
            if(!desktop::resolve_numeric_source_format(candidate,source.id,endpoint->native_sample_rate,endpoint->native_channels,error))return false;
    }
    graph=std::move(candidate);return true;
}

struct NumericEngine::Impl final : application::NumericGraphPreparationPort,application::GraphRuntimePort,application::ControlClock {
    NumericEngine& owner;
    dataflow_runtime::NumericRuntimeProfile profile=desktop_profile();
    ExecutionBoundary execution;
    std::mutex& mutex;
    EngineSnapshot& state;
    dataflow_runtime::GraphExecutor& executor;
    DeliveryHub delivery;
    SourceIngress ingress;
    static bool transport_supported(const domain::ResolvedProcessingNode& node) noexcept {
        if(node.kind!="NetworkInput" && node.kind!="NetworkOutput")return true;
        namespace nd=bdsp_protocol::numeric_data;
        for(const auto& port:node.kind=="NetworkInput"?node.outputs:node.inputs) {
            nd::Message marker;marker.kind=nd::Kind::Unavailable;marker.endpoint={"transport",node.id,nd::Direction::Input,port.id};
            marker.envelope={1,1,1,1,1,1,0};marker.stamp.available=false;marker.stamp.stream_id="transport";
            marker.contract=port.contract.view();
            if(auto* numeric=std::get_if<NumericContract>(&marker.contract)) {
                if(numeric->representation.encoding==StorageEncoding::Native)numeric->representation.encoding=StorageEncoding::FixedLittleEndian;
            }
            if(!nd::measure(marker,nd::desktop_limits()))return false;
        }
        return true;
    }
    domain::NumericDeviceCapabilities device_capabilities() const noexcept {
        return {this,[](const void* context,const domain::ResolvedProcessingNode& node,const domain::ProcessingResourceProfile& resources) noexcept {
            const auto& impl=*static_cast<const Impl*>(context);
            if(!SourceIngress::physical_supported(node) || !transport_supported(node))return domain::ProcessingSupport{};
            const auto actual=dataflow_runtime::PreparedGraph::numeric_capabilities(impl.profile);
            return actual.query(actual.context,node,resources);
        },dataflow_runtime::PreparedGraph::numeric_capabilities(profile).profile};
    }
    application::GraphLifecycle lifecycle{*this,*this,device_capabilities(),{profile.admission,profile.resources}};
    application::GraphLifecycleControlPort runtime{lifecycle};
    application::ProcessingCapabilities caps;
    std::array<application::ControlSessionSlot,8> sessions{};
    std::unique_ptr<application::ProcessingControlService> service;
    application::SessionIdentity local{};
    std::mutex control_mutex;
    std::uint64_t next_reply_check=0,next_diagnostics_ns=0,rate_sample_ns=0,rate_graph_revision=0;
    std::map<domain::NodeId,dataflow_runtime::NodeStats> rate_baselines;
    std::map<std::uint64_t,std::function<bool(const domain::NumericProcessingGraph&)>> control_reply_checks;
    std::unique_lock<std::mutex> pending_commit;
    std::uint64_t control_request=0,public_request=0;
    QTimer timer;
    std::function<void(EngineSnapshot)> snapshot_handler;
    std::function<void(EngineReply)> reply_handler;
    QString network_error;
    std::uint64_t snapshot_generation=0,reply_generation=0;
    Impl(NumericEngine& target,AudioSourceFactory source):owner(target),execution(target,profile),
        mutex(execution.mutex),state(execution.state),executor(execution.executor),delivery(execution),ingress(execution,delivery,std::move(source)) {
        ingress.snapshot_changed=[this]{if(snapshot_handler)snapshot_handler(owner.snapshot());};
        caps.numeric=application::NumericProcessingCapabilities{
            device_capabilities(),{profile.admission,profile.resources}};
        caps.control.max_sessions=sessions.size();
        caps.control.max_prepared_bytes=profile.resources.max_peak_bytes;
        caps.control.max_replay_bytes_per_session=48*1024*1024;
        caps.control.max_replay_bytes_total=384*1024*1024;
        caps.control.max_control_scratch_bytes=48*1024*1024;
        auto epoch=std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
        service=std::make_unique<application::ProcessingControlService>(runtime,*this,caps,sessions,epoch?epoch:1);
        local=service->open_session(1).identity;
        auto reply=command(application::GetState{});project(reply);
        QObject::connect(&timer,&QTimer::timeout,&owner,[this]{QPointer<NumericEngine> lifetime(&owner);refresh_diagnostics();if(lifetime)delivery.drain();});timer.start(16);
    }
    ~Impl(){shutdown();}
    std::uint64_t now_ms() const noexcept override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    application::ProcessingReply command(const application::ProcessingCommand& value) {
        return service->submit(local,{++control_request,state.revision},value);
    }
    void project(const application::ProcessingReply& reply) {
        // Runtime replacement and its public revision become visible atomically.
        auto lock=pending_commit.owns_lock()?std::move(pending_commit):std::unique_lock<std::mutex>(mutex);
        const bool graph_changed=state.graph_revision!=reply.state.graph_revision || state.graph.isEmpty();
        state.epoch=reply.state.epoch;state.revision=reply.state.revision;state.graph_revision=reply.state.graph_revision;
        if(reply.state.applied_numeric && graph_changed) {
            QByteArray bytes;
            if(desktop::encode_numeric_graph(*reply.state.applied_numeric,bytes)) {
                state.graph=QJsonDocument::fromJson(bytes).object();
                auto nodes=state.graph.value("nodes").toArray();
                for(qsizetype i=0;i<nodes.size();++i) {
                    auto node=nodes[i].toObject();if(node.value("kind").toString()!=QStringLiteral("AudioSource"))continue;
                    if(const auto device=ingress.capture_devices.find(node.value("id").toInt());device!=ingress.capture_devices.end()) {
                        node["adapter"]=QJsonObject{{"device_identity",device->second}};nodes[i]=node;
                    }
                }
                state.graph["nodes"]=nodes;
            }
        }
        state.statuses[QStringLiteral("analyzer")]={reply.state.run_requested?State::Running:State::Stopped,{},state.revision};
    }
    bool retained_replies_fit(const domain::NumericProcessingGraph& graph) const {
        for(const auto& [id,check]:control_reply_checks)if(!check(graph))return false;return true;
    }
    Status prepare_numeric(const domain::NumericProcessingGraph& graph,const application::NumericRuntimeLimits& limits,
        application::PreparedExecution& out) override {
        for(const auto& node:graph.nodes) {
            domain::ResolvedProcessingNode resolved;
            if(!domain::resolve_processing_node(node,profile.admission,resolved) || !ingress.physical_supported(resolved) || !transport_supported(resolved))return Status::incompatible_contract;
        }
        if(ingress.native_capture_backend)for(const auto& node:graph.nodes)if(node.kind=="AudioSource") {
            AudioSourceKind kind=AudioSourceKind::SystemOutput;
            if(const auto* parameters=std::get_if<domain::EndpointProcessingParameters>(&node.parameters))
                for(const auto& p:parameters->values)if(p.key=="source")
                    if(const auto* literal=std::get_if<NumericLiteral>(&p.value))kind=AudioSourceKind(std::visit([](auto v){return int(v);},*literal));
            const auto endpoints=enumerate_audio_capture_devices(kind);
            const auto configured=ingress.capture_devices.find(node.id);
            const auto endpoint=std::find_if(endpoints.begin(),endpoints.end(),[&](const auto& d){
                return configured==ingress.capture_devices.end()?d.is_default:d.id==configured->second;
            });
            // Missing endpoint keeps the explicitly known contract for zero
            // advancement. A discovered different format is never silently mixed.
            if(endpoint!=endpoints.end() && endpoint->native_channels && endpoint->native_sample_rate) {
                if(endpoint->native_channels!=node.outputs.size())return Status::incompatible_contract;
                for(const auto& output:node.outputs) {
                    const auto view=output.contract.view();const auto* numeric=std::get_if<NumericContract>(&view);
                    if(!numeric || numeric->clock.sample_rate!=endpoint->native_sample_rate)return Status::incompatible_contract;
                }
            }
        }
        remote_numeric::CapabilitySnapshot candidate_capabilities;
        if(candidate_capabilities.assign(graph,caps)!=Status::ok ||
           !control::numeric::reply_envelope_fits(graph,remote_numeric::control_limits(),candidate_capabilities.value))return Status::exhausted;
        if(!retained_replies_fit(graph))return Status::exhausted;
        dataflow_runtime::GraphExecutor::PreparationBase base;std::size_t retained_capture_bytes=0;
        {std::lock_guard lock(mutex);base=executor.snapshot_preparation_base();retained_capture_bytes=ingress.working_capture_bytes;}
        std::size_t candidate_capture_bytes=0;
        const auto capture_status=ingress.capture_reservation(graph,candidate_capture_bytes);
        if(capture_status!=Status::ok)return capture_status;
        auto platform_limits=limits;
        auto& retained=platform_limits.resources.retained_working_bytes;
        if(retained_capture_bytes>SIZE_MAX-retained)return Status::overflow;
        retained+=retained_capture_bytes;
        if(candidate_capture_bytes>SIZE_MAX-retained)return Status::overflow;
        retained+=candidate_capture_bytes;
        if(retained>platform_limits.resources.max_peak_bytes)return Status::exhausted;
        const auto status=executor.prepare_numeric_detached(graph,platform_limits,base,out);
        if(status!=Status::ok){qWarning("Numeric cold prepare failed: graph=%s status=%d nodes=%zu",graph.graph_id.c_str(),int(status),graph.nodes.size());return status;}
        {std::lock_guard lock(mutex);const auto registered=executor.register_prepared(out,base);if(registered!=Status::ok)qWarning("Numeric cold prepare registration failed: status=%d",int(registered));return registered;}
    }
    Status apply(application::PreparedExecution&& prepared) override {
        if(!prepared.plan || !prepared.plan->numeric_definition())return Status::invalid_argument;
        const auto& graph=*prepared.plan->numeric_definition();
        if(!retained_replies_fit(graph))return Status::exhausted;
        std::unique_lock lock(mutex);
        try {
            SourceIngress::Staged sources;
            DeliveryHub::Staged deliveries;
            auto status=ingress.stage(graph,profile.admission,sources);
            if(status!=Status::ok)return status;
            status=delivery.stage(graph,profile.admission,deliveries);
            if(status!=Status::ok)return status;
            status=executor.apply(std::move(prepared));
            if(status!=Status::ok)return status;
            ingress.commit(std::move(sources));delivery.commit(std::move(deliveries));
            // Keep the same serialization lock until project publishes revision.
            pending_commit=std::move(lock);return Status::ok;
        } catch(const std::bad_alloc&) {return Status::exhausted;}
    }
    Status shutdown() override {
        timer.stop();delivery.preview_timer.stop();ingress.clock_timer.stop();ingress.stop_sources();
        std::lock_guard lock(mutex);delivery.wled_timer.stop();
        const auto status=executor.shutdown();ingress.capture_requested=false;
        ingress.clear();delivery.clear();return status;
    }
    void refresh_diagnostics() {
        const auto now=steady_ns();if(now<next_diagnostics_ns)return;
        next_diagnostics_ns=now+500000000;
        const auto count=[](std::uint64_t value){return QString::number(value);};
        QJsonArray source_diagnostics;
        for(const auto& [node,source]:ingress.sources) {
            const auto d=source.source->diagnostics();
            source_diagnostics.append(QJsonObject{{"node",node},{"backend",d.backend},{"device",d.device},{"endpoint_id",d.endpoint_id},
                {"sample_rate",int(d.sample_rate)},{"delivered_channels",int(d.delivered_channels)},
                {"native_sample_rate",int(d.native_sample_rate)},{"native_channels",int(d.native_channels)},
                {"backend_sample_rate",int(d.backend_sample_rate)},{"backend_channels",int(d.backend_channels)},
                {"native_format",d.native_format},{"backend_format",d.backend_format},{"delivered_format",d.delivered_format},
                {"callback_count",count(d.callback_count)},{"callback_frames",count(d.callback_frames)},
                {"silence_frames",count(d.silence_frames)},{"buffered_frames",count(d.buffered_frames)},
                {"capacity_frames",count(d.capacity_frames)},{"dropped_frames",count(d.dropped_frames)},
                {"discontinuities",count(d.discontinuities)},{"dispatch_latency_ns",count(d.dispatch_latency_ns)}});
        }
        {
            std::lock_guard lock(mutex);QJsonArray nodes,mixers,external,clock_diagnostics;
            if(rate_graph_revision!=state.graph_revision){rate_graph_revision=state.graph_revision;rate_sample_ns=0;rate_baselines.clear();}
            const bool measured=rate_sample_ns && now>rate_sample_ns;
            const double seconds=measured?double(now-rate_sample_ns)/1000000000.:0.;
            double processing_hz=0.;int processing_priority=0;domain::NodeId processing_node=-1;QString processing_basis;
            if(executor.plan())for(const auto& node:executor.plan()->numeric_definition()->nodes)if(const auto stats=executor.node_stats(node.id)) {
                const auto prior=rate_baselines.find(node.id);
                const bool comparable=measured && prior!=rate_baselines.end() && stats->calls>=prior->second.calls && stats->produced>=prior->second.produced && stats->errors>=prior->second.errors && stats->self_nanoseconds>=prior->second.self_nanoseconds;
                const auto calls=comparable?stats->calls-prior->second.calls:0;
                const double calls_hz=comparable?double(calls)/seconds:0.;
                const double produced_hz=comparable?double(stats->produced-prior->second.produced)/seconds:0.;
                const auto errors_delta=comparable?stats->errors-prior->second.errors:0;
                const double busy_fraction=comparable?double(stats->self_nanoseconds-prior->second.self_nanoseconds)/(seconds*1000000000.):0.;
                int priority=0;QString basis;
                if(node.kind=="Window"){priority=4;basis=QStringLiteral("window_outputs");}
                else if(node.kind=="Mixer"){priority=3;basis=QStringLiteral("mixer_outputs");}
                else if(node.kind=="StereoFFT" || node.kind=="Mel" || node.kind=="PcmResampler"){priority=2;basis=QStringLiteral("transform_outputs");}
                else if(node.kind=="AudioSource" || node.kind=="TestSignal" || node.kind=="SignalGenerator" || node.kind=="NetworkInput"){priority=1;basis=QStringLiteral("source_outputs");}
                if(priority>processing_priority || (priority==processing_priority && priority && produced_hz>processing_hz)){
                    processing_priority=priority;processing_hz=produced_hz;processing_node=node.id;processing_basis=basis;}
                rate_baselines[node.id]=*stats;
                QJsonArray histogram;for(auto value:stats->self_histogram)histogram.append(count(value));
                nodes.append(QJsonObject{{"node",node.id},{"calls",count(stats->calls)},{"produced",count(stats->produced)},
                    {"errors",count(stats->errors)},{"self_nanoseconds",count(stats->self_nanoseconds)},
                    {"max_self_nanoseconds",count(stats->max_self_nanoseconds)},
                    {"deadline_misses",count(stats->deadline_misses)},{"self_histogram",histogram},
                    {"calls_hz",calls_hz},{"produced_hz",produced_hz},{"active",calls!=0},
                    {"errors_delta",count(errors_delta)},{"busy_fraction",busy_fraction}});
                for(const auto& port:node.inputs)if(const auto input=executor.mixer_input_stats(node.id,port.id))
                    mixers.append(QJsonObject{{"node",node.id},{"port",int(port.id)},{"depth",count(input->depth)},
                        {"capacity",count(input->capacity)},{"dropped_samples",count(input->dropped_samples)},
                        {"underrun_samples",count(input->underrun_samples)}});
            }
            rate_sample_ns=now;
            for(const auto& [id,clock]:ingress.clocks)clock_diagnostics.append(QJsonObject{{"node",id},{"sample_rate",int(clock.rate)},
                {"lag_samples",count(clock.lag_samples)},{"max_lag_samples",count(clock.max_lag_samples)},{"catchup_limits",count(clock.catchup_limits)}});
            for(const auto& [node,input]:ingress.external_inputs)external.append(QJsonObject{{"node",node},{"missing",input.missing},{"zero_frames",count(input.zero_frames)}});
            state.diagnostics={{"node_stats",nodes},{"sources",source_diagnostics},{"mixer_inputs",mixers},{"external_inputs",external},
                {"sample_clocks",clock_diagnostics},{"processing_measured",measured && processing_node>=0},
                {"processing_hz",processing_hz},{"processing_rate_node",processing_node},{"processing_rate_basis",processing_basis},{"measurement_seconds",seconds},
                {"queues",QJsonObject{{"transport_frames",int(delivery.frames.size())},{"transport_rows",int(delivery.rows.size())},
                    {"preview_frames",int(delivery.previews.size())},{"preview_rows",int(delivery.preview_rows.size())},
                    {"transport_frame_drops",count(delivery.transport_frame_drops)},{"transport_row_drops",count(delivery.transport_row_drops)},
                    {"preview_coalesced",count(delivery.preview_coalesced)},{"preview_row_drops",count(delivery.preview_row_drops)}}}};
        }
        if(snapshot_handler)snapshot_handler(owner.snapshot());
    }
};

NumericEngine::NumericEngine(AudioSourceFactory source,QObject* parent):QObject(parent),impl_(std::make_unique<Impl>(*this,std::move(source))){}
NumericEngine::~NumericEngine()=default;
void NumericEngine::set_adapter_handlers(std::unique_ptr<QObject> owner,AdapterHandlers handlers) {
    adapter_handlers_={};
    adapter_owner_.reset();
    adapter_owner_=std::move(owner);
    adapter_handlers_=std::move(handlers);
}
EnginePort::PortDeliveryMode NumericEngine::port_delivery_mode() const noexcept{return PortDeliveryMode::CanonicalNumericPortsV2;}
bool NumericEngine::subscribe_numeric_frames(std::vector<EndpointIdentity> endpoints,QString* error) {
    if(endpoints.size()>2048){if(error)*error=QStringLiteral("Numeric subscription limit exceeded");return false;}
    std::sort(endpoints.begin(),endpoints.end());endpoints.erase(std::unique(endpoints.begin(),endpoints.end()),endpoints.end());
    {std::lock_guard lock(impl_->mutex);if(impl_->delivery.subscriptions==endpoints)return true;impl_->delivery.subscriptions=std::move(endpoints);impl_->delivery.normalize_previews_locked();impl_->delivery.frame_active->store(false,std::memory_order_release);impl_->delivery.history_active->store(false,std::memory_order_release);impl_->delivery.frame_active=std::make_shared<std::atomic_bool>(true);impl_->delivery.history_active=std::make_shared<std::atomic_bool>(true);}
    impl_->ingress.reproject_failed_sources();return true;
}
void NumericEngine::set_numeric_frame_handler(std::function<void(DeliveredNumericFrame)> handler){std::lock_guard lock(impl_->mutex);impl_->delivery.frame_active->store(false,std::memory_order_release);impl_->delivery.frame_active=std::make_shared<std::atomic_bool>(true);impl_->delivery.frame_handler=std::move(handler);}
void NumericEngine::set_numeric_history_handler(std::function<void(DeliveredNumericHistoryRow)> handler){std::lock_guard lock(impl_->mutex);impl_->delivery.history_active->store(false,std::memory_order_release);impl_->delivery.history_active=std::make_shared<std::atomic_bool>(true);impl_->delivery.history_handler=std::move(handler);}
void NumericEngine::set_numeric_preview_cadence_hz(int hz){
    impl_->delivery.preview_timer.stop();
    std::lock_guard lock(impl_->mutex);
    impl_->delivery.preview_interval_ms=std::max(1,1000/std::clamp(hz,1,240));
    impl_->delivery.normalize_previews_locked();
    impl_->delivery.preview_last_flush_ns=0;
    if(impl_->delivery.preview_scheduled)QMetaObject::invokeMethod(this,[this]{impl_->delivery.dispatch_previews();},Qt::QueuedConnection);
}
int NumericEngine::numeric_preview_interval_ms() const{return impl_->delivery.preview_interval_ms;}
void NumericEngine::set_snapshot_handler(std::function<void(EngineSnapshot)> handler){++impl_->snapshot_generation;impl_->snapshot_handler=std::move(handler);}
void NumericEngine::set_reply_handler(std::function<void(EngineReply)> handler){++impl_->reply_generation;impl_->reply_handler=std::move(handler);}
EngineSnapshot NumericEngine::snapshot() const {std::lock_guard lock(impl_->mutex);return impl_->state;}
QString NumericEngine::network_warning() const{return adapter_handlers_.warning?adapter_handlers_.warning():impl_->network_error;}
bool NumericEngine::configure_network(QJsonObject config,QString* error){
    return !adapter_handlers_.configure_network || adapter_handlers_.configure_network(config,error);
}
std::uint64_t NumericEngine::register_control_reply_check(std::function<bool(const domain::NumericProcessingGraph&)> check) {
    if(!check || impl_->control_reply_checks.size()>=impl_->sessions.size())return 0;
    if(const auto* working=impl_->runtime.working_numeric_graph();working && !check(*working))return 0;
    const auto token=++impl_->next_reply_check;impl_->control_reply_checks.emplace(token,std::move(check));return token;
}
void NumericEngine::unregister_control_reply_check(std::uint64_t token){impl_->control_reply_checks.erase(token);}
application::ProcessingCapabilities NumericEngine::capabilities() const {return impl_->caps;}
domain::ProcessingDiagnostic NumericEngine::validate_numeric_node(const domain::ConfiguredProcessingNode& node) const {
    std::lock_guard lock(impl_->mutex);
    domain::ResolvedProcessingNode resolved;
    const auto diagnostic=domain::resolve_processing_node(node,impl_->profile.admission,resolved);
    if(!diagnostic)return diagnostic;
    const auto capabilities=impl_->device_capabilities();
    if(!capabilities.query || !capabilities.query(capabilities.context,resolved,impl_->profile.resources).supported)
        return {domain::ProcessingError::UnsupportedCombination,node.id};
    return {};
}

Status NumericEngine::feed_numeric(domain::NodeId node,const DataFrameView& frame) {
    std::lock_guard lock(impl_->mutex);auto delivered=frame;
    if(const auto source=impl_->ingress.external_inputs.find(node);source!=impl_->ingress.external_inputs.end() && source->second.missing && frame.stamp.available)
        delivered.stamp.discontinuity=true;
    const auto result=impl_->ingress.process(node,delivered);impl_->ingress.note_external(node,delivered,result);return result;
}
Status NumericEngine::feed_numeric_owned(domain::NodeId node,const OwnedDataFrame& frame,
    std::optional<std::uint64_t> ordinal,std::shared_ptr<const void> retention_owner,std::optional<std::uint64_t> filled_count) {
    std::lock_guard lock(impl_->mutex);auto delivered=frame;
    if(const auto source=impl_->ingress.external_inputs.find(node);source!=impl_->ingress.external_inputs.end() && source->second.missing && frame.view().stamp.available)
        delivered=frame.with_discontinuity();
    const auto result=impl_->executor.process_numeric_owned_ports(node,delivered,{impl_->state.graph_revision,impl_->state.epoch},
        impl_->delivery.callbacks(),ordinal,std::move(retention_owner),filled_count);
    impl_->ingress.note_external(node,delivered.view(),result);return result;
}
void NumericEngine::synchronize_control_state(const application::ProcessingReply& reply) {
    impl_->project(reply);
    std::lock_guard lock(impl_->mutex);impl_->ingress.capture_requested=reply.state.run_requested;
}
void NumericEngine::with_control_service(const std::function<void(application::ProcessingControlService&)>& call) {
    bool changed=false;
    {std::lock_guard control_lock(impl_->control_mutex);
        const auto prior=snapshot();
        call(*impl_->service);const auto reply=impl_->command(application::GetState{});
        impl_->project(reply);
        {std::lock_guard lock(impl_->mutex);impl_->ingress.capture_requested=reply.state.run_requested;
            changed=prior.graph_revision!=impl_->state.graph_revision;}}
    if(changed){impl_->ingress.reconcile_sources();if(impl_->snapshot_handler)impl_->snapshot_handler(snapshot());}
}
std::uint64_t NumericEngine::subscribe_numeric_tap(std::vector<EndpointIdentity> endpoints,
    std::function<void(DeliveredNumericFrame)> frame,std::function<void(DeliveredNumericHistoryRow)> history) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->delivery.taps.size()>=8 || endpoints.size()>2048)return 0;
    const auto token=++impl_->delivery.next_tap;
    impl_->delivery.taps.emplace(token,DeliveryHub::Tap{std::make_shared<std::atomic_bool>(true),std::move(endpoints),std::move(frame),std::move(history)});return token;
}
void NumericEngine::update_numeric_tap(std::uint64_t token,std::vector<EndpointIdentity> endpoints) {
    if(endpoints.size()>2048)return;
    if(endpoints.empty())endpoints.push_back({"",-1,domain::PortDirection::Output,0});
    std::lock_guard lock(impl_->mutex);auto it=impl_->delivery.taps.find(token);if(it==impl_->delivery.taps.end()||it->second.endpoints==endpoints)return;
    it->second.active->store(false,std::memory_order_release);it->second.active=std::make_shared<std::atomic_bool>(true);it->second.endpoints=std::move(endpoints);
}
void NumericEngine::unsubscribe_numeric_tap(std::uint64_t token) {std::lock_guard lock(impl_->mutex);if(auto it=impl_->delivery.taps.find(token);it!=impl_->delivery.taps.end()){it->second.active->store(false,std::memory_order_release);impl_->delivery.taps.erase(it);}}
Status NumericEngine::query_numeric(const EndpointIdentity& endpoint,DeliveredNumericFrame& out,std::uint64_t* ordinal,std::uint64_t* filled_count,bool* is_history) {
    if(is_history)*is_history=false;
    const auto wait_start=core::perf::enabled.load(std::memory_order_relaxed)?core::perf::now():0;
    std::lock_guard lock(impl_->mutex);
    if(wait_start)core::perf::record(core::perf::Stage::NumericQueryWait,wait_start,core::perf::now()-wait_start);
    core::perf::Scope hold(core::perf::Stage::NumericQueryHold);
    const auto context=impl_->delivery.contexts.find(endpoint);
    if(context==impl_->delivery.contexts.end() || !impl_->executor.plan() || !impl_->executor.context())return Status::unavailable;
    const auto history=impl_->delivery.history_ordinals.find(endpoint);
    if(history==impl_->delivery.history_ordinals.end())return Status::unsupported_version;
    DataFrameView borrowed;
    std::uint64_t occupancy=0,accepted_ordinal=0;
    const auto result=impl_->executor.plan()->history(endpoint.node,endpoint.port,*impl_->executor.context(),borrowed,&occupancy,&accepted_ordinal);
    if(result==Status::incompatible_contract){
        const auto cached=impl_->delivery.latest_matrices.find(endpoint);
        if(cached==impl_->delivery.latest_matrices.end() || !cached->second.frame)return Status::unavailable;
        out=cached->second;out.context=context->second;out.delivery={impl_->state.graph_revision,impl_->state.epoch};
        if(ordinal)*ordinal=0;if(filled_count)*filled_count=0;return out.status;
    }
    if(result!=Status::ok && result!=Status::unavailable)return result;
    if(is_history)*is_history=true;
    borrowed.stamp.graph_revision=impl_->state.graph_revision;borrowed.stamp.epoch=impl_->state.epoch;
    DeliveredNumericFrame candidate{context->second,{impl_->state.graph_revision,impl_->state.epoch},{},result};
    Status retained;
    {core::perf::Scope copy(core::perf::Stage::NumericQueryCopy);
        retained=retain_frame(borrowed,frame_limits,impl_->delivery.arena->payload_pool,impl_->delivery.arena->metadata_pool,candidate.frame);}
    if(retained!=Status::ok)return retained;
    out=std::move(candidate);if(ordinal)*ordinal=accepted_ordinal;if(filled_count)*filled_count=occupancy;return result;
}
quint64 NumericEngine::submit(EngineCommand request) {
    const auto id=++impl_->public_request;EngineReply reply;reply.request_id=id;bool reconcile=false;
    QString adapter_error;
    bool adapter_valid=request.kind!=EngineCommand::ApplyGraph || !adapter_handlers_.validate_graph ||
        adapter_handlers_.validate_graph(request.graph,&adapter_error);
    std::map<domain::NodeId,QString> capture_devices;
    if(request.kind==EngineCommand::ApplyGraph && adapter_valid)for(const auto value:request.graph.value("nodes").toArray()) {
        const auto node=value.toObject();if(node.value("kind").toString()!=QStringLiteral("AudioSource"))continue;
        QJsonObject adapter;const auto raw=node.value("adapter");if(raw.isUndefined())continue;
        const auto decoded=desktop::decode_numeric_node_adapter(QJsonDocument(raw.toObject()).toJson(QJsonDocument::Compact),adapter);
        if(!raw.isObject() || !decoded){adapter_valid=false;adapter_error=QStringLiteral("Invalid capture endpoint adapter");break;}
        const auto device=adapter.value("device_identity").toString();if(!device.isEmpty())capture_devices.emplace(node.value("id").toInt(),device);
    }
    {
        std::lock_guard control_lock(impl_->control_mutex);
        impl_->project(impl_->command(application::GetState{}));
        application::ProcessingReply result;
        if(request.kind==EngineCommand::ApplyGraph) {
            domain::NumericProcessingGraph graph;
            const auto decoded=desktop::decode_numeric_graph(QJsonDocument(request.graph).toJson(QJsonDocument::Compact),graph);
            if(!adapter_valid)reply.error=adapter_error;
            else if(!decoded)reply.error=decoded.path+": "+decoded.error;
            else if(request.expected_revision!=std::numeric_limits<quint64>::max() && request.expected_revision!=impl_->state.graph_revision)
                reply.error=QStringLiteral("Graph revision conflict");
            else {
                // Stage platform selection for cold admission; rollback is atomic
                // with the candidate graph and never changes the active capture.
                auto prior_devices=std::move(impl_->ingress.capture_devices);
                impl_->ingress.capture_devices=std::move(capture_devices);
                application::ProcessingDTO dto;dto.numeric=graph;
                result=impl_->command(application::PrepareProcessing{&dto});
                impl_->project(result);
                if(result.accepted)result=impl_->command(application::ApplyProcessing{result.prepared_token,graph.graph_id});
                if(!result.accepted)impl_->ingress.capture_devices=std::move(prior_devices);
                impl_->project(result);reply.accepted=result.accepted;reconcile=result.accepted;
                if(result.accepted)impl_->ingress.capture_requested=result.state.run_requested;
                if(!result.accepted)reply.error=failure(result.status)+QStringLiteral(" control=%1 validation=%2 node=%3").arg(int(result.error)).arg(int(result.numeric_validation.error)).arg(result.numeric_validation.node);
            }
        } else if(request.kind==EngineCommand::Snapshot || request.kind==EngineCommand::Subscribe)reply.accepted=true;
        else reply.error=QStringLiteral("Processing parameters belong to the numeric graph");
        reply.snapshot=impl_->state;
    }
    if(request.kind==EngineCommand::ApplyGraph && reply.accepted && adapter_handlers_.commit_graph)
        adapter_handlers_.commit_graph(request.graph);
    if(reconcile)impl_->ingress.reconcile_sources();
    const auto snapshot_generation=impl_->snapshot_generation,reply_generation=impl_->reply_generation;
    QMetaObject::invokeMethod(this,[this,reply=std::move(reply),snapshot_generation,reply_generation]() mutable {
        QPointer<NumericEngine> lifetime(this);
        const auto current=snapshot();
        if(impl_->snapshot_handler && snapshot_generation==impl_->snapshot_generation && current.epoch==reply.snapshot.epoch && current.revision==reply.snapshot.revision)impl_->snapshot_handler(reply.snapshot);
        if(!lifetime)return;
        // Request completion remains observable; its presentation uses live state.
        reply.snapshot=current;
        if(impl_->reply_handler && reply_generation==impl_->reply_generation)impl_->reply_handler(std::move(reply));
    },Qt::QueuedConnection);
    return id;
}
}

