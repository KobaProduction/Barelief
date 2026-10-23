#include <desktop/numeric_source_format.h>
#include "numeric_engine_ingress.h"
#include <pipeline/numeric_engine.h>
#include <desktop/numeric_project_document.h>
#include <control/numeric_remote_support.h>
#include <dataflow_runtime/runtime.h>
#include <speex_backend.h>
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
    constexpr std::size_t fixed=256*(262144+16384)+8*(524288+16384)+(384+48)*1024*1024+remote_numeric::ControlTransportReservationBytes;
    auto profile=dataflow_runtime::NumericRuntimeProfile{{512,2048,16,32,1024*1024,4*1024*1024,frame_limits},
        {{16*1024*1024,32*1024*1024,16*1024*1024,1024*1024},fixed,576*1024*1024+remote_numeric::ControlTransportReservationBytes},
        {32768,8,262144,262144,16*1024*1024},{true,true,true},true};
    profile.music_enabled=true;
    profile.experimental_resampling_enabled=true;
    profile.resampler_backend=&desktop_resampling::speex_backend_factory();
    profile.monotonic_nanoseconds=[](void*) noexcept -> std::uint64_t {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    return profile;
}

}

namespace {
std::optional<AudioSourceKind> physical_capture_kind(const domain::ConfiguredProcessingNode& source) {
    auto kind=AudioSourceKind::SystemOutput;
    const auto* parameters=std::get_if<domain::EndpointProcessingParameters>(&source.parameters);
    if(!parameters)return kind;
    const auto parameter=std::find_if(parameters->values.begin(),parameters->values.end(),
        [](const auto& value){return value.key=="source";});
    if(parameter!=parameters->values.end())
        if(const auto* literal=std::get_if<NumericLiteral>(&parameter->value))
            kind=AudioSourceKind(std::visit([](auto value){return int(value);},*literal));
    if(kind==AudioSourceKind::SystemOutput || kind==AudioSourceKind::Microphone)return kind;
    return {};
}
bool resolve_capture_source(domain::NumericProcessingGraph& graph,const domain::ConfiguredProcessingNode& source,
    const domain::ProcessingAdmissionLimits& admission,const std::map<domain::NodeId,QString>& devices,QString* error) {
    const auto kind=physical_capture_kind(source);
    if(!kind){if(error)*error=QStringLiteral("Invalid physical source kind");return false;}
    const auto endpoints=enumerate_audio_capture_devices(*kind);
    const auto configured=devices.find(source.id);
    const auto endpoint=std::find_if(endpoints.begin(),endpoints.end(),[&](const auto& device){
        return configured==devices.end()?device.is_default:device.id==configured->second;
    });
    if(endpoint==endpoints.end() || !endpoint->native_sample_rate || !endpoint->native_channels)return true;
    return desktop::resolve_numeric_source_format(graph,source.id,endpoint->native_sample_rate,
        endpoint->native_channels,admission,error);
}
}
bool resolve_native_capture_graph(domain::NumericProcessingGraph& graph,
    const domain::ProcessingAdmissionLimits& admission,
    const std::map<domain::NodeId,QString>& devices,QString* error) {
    if(error)error->clear();
    std::size_t owned=0;
    if(domain::processing_graph_owned_bytes(graph,admission,owned)!=signal_contracts::Status::ok) {
        if(error)*error=QStringLiteral("Native source graph exceeds execution admission limits");
        return false;
    }
    auto candidate=graph;
    for(const auto& source:graph.nodes) {
        if(source.kind=="AudioSource" && !resolve_capture_source(candidate,source,admission,devices,error))return false;
    }
    graph=std::move(candidate);
    return true;
}

struct NumericEngine::Impl final : application::NumericGraphPreparationPort,application::GraphRuntimePort,application::ControlClock {
    NumericEngine& owner;
    const dataflow_runtime::NumericRuntimeProfile profile=desktop_profile();
    ExecutionBoundary execution;
    std::mutex& mutex;
    EngineSnapshot& state;
    dataflow_runtime::GraphExecutor& executor;
    DeliveryHub delivery;
    SourceIngress ingress;
    static bool transport_supported(const domain::ResolvedProcessingNode& node) noexcept {
        if(node.kind!="NetworkInput" && node.kind!="NetworkOutput")return true;
        for(const auto& port:node.kind=="NetworkInput"?node.outputs:node.inputs) {
            if(!transport_port_supported(node.id,port))return false;
        }
        return true;
    }
    static bool transport_port_supported(domain::NodeId node,const domain::ProcessingPort& port) noexcept {
        namespace nd=bdsp_protocol::numeric_data;
        nd::Message marker;marker.kind=nd::Kind::Unavailable;marker.endpoint={"transport",node,nd::Direction::Input,port.id};
        marker.envelope={1,1,1,1,1,1,0};marker.stamp.available=false;marker.stamp.stream_id="transport";
        marker.contract=port.contract.view();
        if(auto* numeric=std::get_if<NumericContract>(&marker.contract)) {
            if(numeric->representation.encoding==StorageEncoding::Native)numeric->representation.encoding=StorageEncoding::FixedLittleEndian;
        }
        return bool(nd::measure(marker,nd::desktop_limits()));
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
        initialize_control_service();
        auto reply=command(application::GetState{});project(reply);
        initialize_diagnostics_timer();
    }
    void initialize_control_service() {
        caps.numeric=application::NumericProcessingCapabilities{
            device_capabilities(),{profile.admission,profile.resources}};
        caps.control.max_sessions=sessions.size();
        caps.control.max_field_string_bytes=remote_numeric::control_limits().string_bytes;
        caps.control.max_prepared_bytes=profile.resources.max_peak_bytes;
        caps.control.max_replay_bytes_per_session=48*1024*1024;
        caps.control.max_replay_bytes_total=384*1024*1024;
        caps.control.max_control_scratch_bytes=48*1024*1024;
        auto epoch=std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
        service=std::make_unique<application::ProcessingControlService>(runtime,*this,caps,sessions,epoch?epoch:1);
        local=service->open_session(1).identity;
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
        if(reply.state.applied_numeric && graph_changed)project_graph(*reply.state.applied_numeric);
        state.statuses[QStringLiteral("analyzer")]={reply.state.run_requested?State::Running:State::Stopped,{},state.revision};
    }
    void project_graph(const domain::NumericProcessingGraph& graph) {
        QByteArray bytes;
        if(!desktop::encode_numeric_graph(graph,bytes))return;
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
    bool retained_replies_fit(const domain::NumericProcessingGraph& graph) const {
        for(const auto& [id,check]:control_reply_checks)if(!check(graph))return false;return true;
    }
    void initialize_diagnostics_timer() {
        QObject::connect(&timer,&QTimer::timeout,&owner,[this]{
            QPointer<NumericEngine> lifetime(&owner);refresh_diagnostics();if(lifetime)delivery.drain();
        });
        timer.start(16);
    }
    Status prepare_platform_graph(const domain::NumericProcessingGraph& graph,const application::NumericRuntimeLimits& limits,application::PreparedExecution& out) {
        dataflow_runtime::GraphExecutor::PreparationBase base;
        auto platform_limits=limits;
        const auto capture_status=prepare_capture_limits(graph,base,platform_limits);
        if(capture_status!=Status::ok)return capture_status;
        const auto status=executor.prepare_numeric_detached(graph,platform_limits,base,out);
        if(status!=Status::ok){qWarning("Numeric cold prepare failed: graph=%s status=%d nodes=%zu",graph.graph_id.c_str(),int(status),graph.nodes.size());return status;}
        {std::lock_guard lock(mutex);const auto registered=executor.register_prepared(out,base);if(registered!=Status::ok)qWarning("Numeric cold prepare registration failed: status=%d",int(registered));return registered;}
    }
    Status prepare_numeric(const domain::NumericProcessingGraph& requested,const application::NumericRuntimeLimits& limits,
        application::PreparedExecution& out) override {
        domain::NumericProcessingGraph graph,executable;
        if(!domain::resolve_numeric_parameter_bindings(requested,profile.admission,graph,executable))return Status::incompatible_contract;
        if(!graph_nodes_supported(graph)||!native_capture_formats_supported(graph))return Status::incompatible_contract;
        remote_numeric::CapabilitySnapshot candidate_capabilities;
        if(candidate_capabilities.assign(graph,caps)!=Status::ok ||
           !control::numeric::reply_envelope_fits(graph,remote_numeric::control_limits(),candidate_capabilities.value))return Status::exhausted;
        if(!retained_replies_fit(graph))return Status::exhausted;
        return prepare_platform_graph(graph,limits,out);
    }
    bool graph_nodes_supported(const domain::NumericProcessingGraph& graph) const {
        for(const auto& node:graph.nodes) {
            domain::ResolvedProcessingNode resolved;
            if(!domain::resolve_processing_node(node,profile.admission,resolved) ||
               !ingress.physical_supported(resolved) || !transport_supported(resolved))return false;
        }
        return true;
    }
    bool native_capture_formats_supported(const domain::NumericProcessingGraph& graph) const {
        if(!ingress.native_capture_backend)return true;
        for(const auto& node:graph.nodes)
            if(node.kind=="AudioSource"&&!native_capture_format_supported(node))return false;
        return true;
    }
    static AudioSourceKind native_capture_kind(const domain::ConfiguredProcessingNode& node) {
        AudioSourceKind kind=AudioSourceKind::SystemOutput;
        if(const auto* parameters=std::get_if<domain::EndpointProcessingParameters>(&node.parameters))
            for(const auto& parameter:parameters->values)if(parameter.key=="source")
                if(const auto* literal=std::get_if<NumericLiteral>(&parameter.value))
                    kind=AudioSourceKind(std::visit([](auto value){return int(value);},*literal));
        return kind;
    }
    bool native_capture_format_supported(const domain::ConfiguredProcessingNode& node) const {
        const auto kind=native_capture_kind(node);
        const auto endpoints=enumerate_audio_capture_devices(kind);
        const auto configured=ingress.capture_devices.find(node.id);
        const auto endpoint=std::find_if(endpoints.begin(),endpoints.end(),[&](const auto& device){
            return configured==ingress.capture_devices.end()?device.is_default:device.id==configured->second;
        });
        if(endpoint==endpoints.end() || !endpoint->native_channels || !endpoint->native_sample_rate)return true;
        if(desktop::numeric_source_output_count(node,endpoint->native_channels)!=domain::data_outputs(node).size())return false;
        for(const auto& output:node.outputs) {
            const auto view=output.contract.view();const auto* numeric=std::get_if<NumericContract>(&view);
            if(!numeric || numeric->clock.sample_rate!=endpoint->native_sample_rate)return false;
        }
        return true;
    }
    Status prepare_capture_limits(const domain::NumericProcessingGraph& graph,
        dataflow_runtime::GraphExecutor::PreparationBase& base,application::NumericRuntimeLimits& limits) {
        std::size_t retained_capture_bytes=0;
        {std::lock_guard lock(mutex);base=executor.snapshot_preparation_base();retained_capture_bytes=ingress.working_capture_bytes;}
        std::size_t candidate_capture_bytes=0;
        const auto status=ingress.capture_reservation(graph,candidate_capture_bytes);
        if(status!=Status::ok)return status;
        auto& retained=limits.resources.retained_working_bytes;
        if(retained_capture_bytes>SIZE_MAX-retained)return Status::overflow;
        retained+=retained_capture_bytes;
        if(candidate_capture_bytes>SIZE_MAX-retained)return Status::overflow;
        retained+=candidate_capture_bytes;
        return retained>limits.resources.max_peak_bytes?Status::exhausted:Status::ok;
    }
    Status apply(application::PreparedExecution&& prepared) override {
        if(!prepared.plan || !prepared.plan->numeric_definition())return Status::invalid_argument;
        const auto& graph=*prepared.plan->numeric_definition();
        if(!retained_replies_fit(graph))return Status::exhausted;
        std::unique_lock lock(mutex);
        try {
            SourceIngress::Staged sources;
            DeliveryHub::Staged deliveries;
            const auto status=stage_and_apply(graph,std::move(prepared),sources,deliveries);
            if(status!=Status::ok)return status;
            ingress.commit(std::move(sources));delivery.commit(std::move(deliveries));
            // Keep the same serialization lock until project publishes revision.
            pending_commit=std::move(lock);return Status::ok;
        } catch(const std::bad_alloc&) {return Status::exhausted;}
    }
    Status stage_and_apply(const domain::NumericProcessingGraph& graph,application::PreparedExecution&& prepared,
                           SourceIngress::Staged& sources,DeliveryHub::Staged& deliveries) {
        auto status=ingress.stage(graph,profile.admission,sources);
        if(status!=Status::ok)return status;
        status=delivery.stage(graph,profile.admission,deliveries);
        if(status!=Status::ok)return status;
        return executor.apply(std::move(prepared));
    }
    Status shutdown() override {
        timer.stop();delivery.preview_timer.stop();ingress.clock_timer.stop();ingress.stop_sources();
        std::lock_guard lock(mutex);delivery.wled_timer.stop();
        const auto status=executor.shutdown();ingress.capture_requested=false;
        ingress.clear();delivery.clear();return status;
    }
    Status query_cached_matrix(const EndpointIdentity& endpoint,const std::shared_ptr<const NumericDeliveryContext>& context,
                               DeliveredNumericFrame& out,std::uint64_t* ordinal,std::uint64_t* filled_count) {
        const auto cached=delivery.latest_matrices.find(endpoint);
        if(cached==delivery.latest_matrices.end() || !cached->second.frame)return Status::unavailable;
        out=cached->second;out.context=context;out.delivery={state.graph_revision,state.epoch};
        if(ordinal)*ordinal=0;if(filled_count)*filled_count=0;return out.status;
    }
    Status retain_history_query(const DataFrameView& borrowed,DeliveredNumericFrame& candidate) {
        Status retained;
        {core::perf::Scope copy(core::perf::Stage::NumericQueryCopy);
            retained=retain_frame(borrowed,frame_limits,delivery.history_snapshot_arena->payload_pool,
                                  delivery.history_snapshot_arena->metadata_pool,candidate.frame);}
        return retained;
    }
    Status retain_history_result(DataFrameView borrowed,const std::shared_ptr<const NumericDeliveryContext>& context,
        Status result,DeliveredNumericFrame& out,std::uint64_t* ordinal,std::uint64_t* filled_count,std::uint64_t accepted_ordinal,std::uint64_t occupancy) {
        borrowed.stamp.graph_revision=state.graph_revision;borrowed.stamp.epoch=state.epoch;
        DeliveredNumericFrame candidate{context,{state.graph_revision,state.epoch},{},result};
        const auto retained=retain_history_query(borrowed,candidate);
        if(retained!=Status::ok)return retained;
        out=std::move(candidate);if(ordinal)*ordinal=accepted_ordinal;if(filled_count)*filled_count=occupancy;return result;
    }
    Status query_history(const EndpointIdentity& endpoint,const std::shared_ptr<const NumericDeliveryContext>& context,
                         DeliveredNumericFrame& out,std::uint64_t* ordinal,std::uint64_t* filled_count,bool* is_history) {
        DataFrameView borrowed;
        std::uint64_t occupancy=0,accepted_ordinal=0;
        const auto result=executor.plan()->history(endpoint.node,endpoint.port,*executor.context(),borrowed,&occupancy,&accepted_ordinal);
        if(result==Status::incompatible_contract)return query_cached_matrix(endpoint,context,out,ordinal,filled_count);
        if(result!=Status::ok && result!=Status::unavailable)return result;
        if(is_history)*is_history=true;
        return retain_history_result(borrowed,context,result,out,ordinal,filled_count,accepted_ordinal,occupancy);
    }
    bool collect_capture_device(const QJsonObject& node,std::map<domain::NodeId,QString>& devices,QString& error) const {
            const auto raw=node.value("adapter");if(raw.isUndefined())return true;
            QJsonObject adapter;
            const auto decoded=desktop::decode_numeric_node_adapter(
                QJsonDocument(raw.toObject()).toJson(QJsonDocument::Compact),adapter);
            if(!raw.isObject() || !decoded){error=QStringLiteral("Invalid capture endpoint adapter");return false;}
            const auto device=adapter.value("device_identity").toString();
            if(!device.isEmpty())devices.emplace(node.value("id").toInt(),device);
        return true;
    }
    bool collect_capture_devices(const QJsonObject& candidate,std::map<domain::NodeId,QString>& devices,QString& error) const {
        const auto nodes=candidate.value("nodes").toArray();
        for(const auto& value:nodes) {
            const auto node=value.toObject();if(node.value("kind").toString()!=QStringLiteral("AudioSource"))continue;
            if(!collect_capture_device(node,devices,error))return false;
        }
        return true;
    }
    application::ProcessingReply prepare_and_apply_public_graph(const domain::NumericProcessingGraph& graph) {
        application::ProcessingDTO dto;dto.numeric=graph;
        auto result=command(application::PrepareProcessing{&dto});project(result);
        if(result.accepted)result=command(application::ApplyProcessing{result.prepared_token,graph.graph_id});
        return result;
    }
    void commit_public_graph(const domain::NumericProcessingGraph& graph,EngineReply& reply,bool& reconcile,std::map<domain::NodeId,QString> capture_devices) {
        auto prior_devices=std::move(ingress.capture_devices);
        ingress.capture_devices=std::move(capture_devices);
        auto result=prepare_and_apply_public_graph(graph);
        if(!result.accepted)ingress.capture_devices=std::move(prior_devices);
        project(result);reply.accepted=result.accepted;reconcile=result.accepted;
        if(result.accepted)ingress.capture_requested=result.state.run_requested;
        if(!result.accepted)record_public_apply_failure(result,reply);
    }
    void dispatch_public_command(const EngineCommand& request,EngineReply& reply,bool& reconcile,
        std::map<domain::NodeId,QString> capture_devices,bool adapter_valid,const QString& adapter_error) {
    {
        std::lock_guard control_lock(control_mutex);
        project(command(application::GetState{}));
        if(request.kind==EngineCommand::ApplyGraph)
            apply_public_graph(request,reply,reconcile,std::move(capture_devices),adapter_valid,adapter_error);
        else if(request.kind==EngineCommand::Snapshot || request.kind==EngineCommand::Subscribe)reply.accepted=true;
        else reply.error=QStringLiteral("Processing parameters belong to the numeric graph");
        reply.snapshot=state;
    }
    }
    void apply_public_graph(const EngineCommand& request,EngineReply& reply,bool& reconcile,
                            std::map<domain::NodeId,QString> capture_devices,bool adapter_valid,const QString& adapter_error) {
        domain::NumericProcessingGraph graph;
        const auto decoded=desktop::decode_numeric_graph(QJsonDocument(request.graph).toJson(QJsonDocument::Compact),graph);
        if(!adapter_valid){reply.error=adapter_error;return;}
        if(!decoded){reply.error=decoded.path+": "+decoded.error;return;}
        if(request.expected_revision!=std::numeric_limits<quint64>::max() && request.expected_revision!=state.graph_revision){
            reply.error=QStringLiteral("Graph revision conflict");return;}
        commit_public_graph(graph,reply,reconcile,std::move(capture_devices));
    }
    static void record_public_apply_failure(const application::ProcessingReply& result,EngineReply& reply) {
        reply.error=failure(result.status)+QStringLiteral(" control=%1 validation=%2 node=%3")
            .arg(int(result.error)).arg(int(result.numeric_validation.error)).arg(result.numeric_validation.node);
        if(result.numeric_validation.error!=domain::ProcessingError::None)
            reply.numeric_diagnostic=EngineDiagnostic::own(result.numeric_validation);
    }
    void queue_public_reply(EngineReply reply) {
        const auto snapshot_version=snapshot_generation,reply_version=reply_generation;
        QMetaObject::invokeMethod(&owner,[this,reply=std::move(reply),snapshot_version,reply_version]() mutable {
            QPointer<NumericEngine> lifetime(&owner);
            const auto current=owner.snapshot();
            if(snapshot_handler && snapshot_version==snapshot_generation && current.epoch==reply.snapshot.epoch &&
               current.revision==reply.snapshot.revision)snapshot_handler(reply.snapshot);
            if(!lifetime)return;
            reply.snapshot=current;
            if(reply_handler && reply_version==reply_generation)reply_handler(std::move(reply));
        },Qt::QueuedConnection);
    }
    void refresh_diagnostics() {
        const auto now=steady_ns();if(now<next_diagnostics_ns)return;
        next_diagnostics_ns=now+500000000;
        const auto source_diagnostics=collect_source_diagnostics();
        {std::lock_guard lock(mutex);update_runtime_diagnostics(now,source_diagnostics);}
        if(snapshot_handler)snapshot_handler(owner.snapshot());
    }
    QJsonArray collect_source_diagnostics() const {
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
        return source_diagnostics;
    }
    struct RateSummary {bool measured=false;double seconds=0.,hz=0.;int priority=0;domain::NodeId node=-1;QString basis;};
    struct NodeRate {std::uint64_t calls=0,errors=0;double calls_hz=0.,produced_hz=0.,busy_fraction=0.;};
    static std::pair<int,QString> processing_rate_role(std::string_view kind) {
        if(kind=="Window")return {4,QStringLiteral("window_outputs")};
        if(kind=="Mixer")return {3,QStringLiteral("mixer_outputs")};
        if(kind=="StereoFFT" || kind=="Mel" || kind=="PcmResampler")return {2,QStringLiteral("transform_outputs")};
        if(kind=="AudioSource" || kind=="TestSignal" || kind=="SignalGenerator" || kind=="NetworkInput")
            return {1,QStringLiteral("source_outputs")};
        return {};
    }
    NodeRate calculate_node_rate(domain::NodeId node,const dataflow_runtime::NodeStats& stats,const RateSummary& summary) const {
        const auto prior=rate_baselines.find(node);
        const bool comparable=summary.measured && prior!=rate_baselines.end() &&
            stats.calls>=prior->second.calls && stats.produced>=prior->second.produced &&
            stats.errors>=prior->second.errors && stats.self_nanoseconds>=prior->second.self_nanoseconds;
        if(!comparable)return {};
        NodeRate result;
        result.calls=stats.calls-prior->second.calls;
        result.errors=stats.errors-prior->second.errors;
        result.calls_hz=double(result.calls)/summary.seconds;
        result.produced_hz=double(stats.produced-prior->second.produced)/summary.seconds;
        result.busy_fraction=double(stats.self_nanoseconds-prior->second.self_nanoseconds)/(summary.seconds*1000000000.);
        return result;
    }
    void append_node_statistics(const domain::ConfiguredProcessingNode& node,const dataflow_runtime::NodeStats& stats,
                                const NodeRate& rate,QJsonArray& nodes,QJsonArray& mixers) {
        const auto count=[](std::uint64_t value){return QString::number(value);};
        QJsonArray histogram;for(auto value:stats.self_histogram)histogram.append(count(value));
        nodes.append(QJsonObject{{"node",node.id},{"calls",count(stats.calls)},{"produced",count(stats.produced)},
            {"errors",count(stats.errors)},{"self_nanoseconds",count(stats.self_nanoseconds)},
            {"max_self_nanoseconds",count(stats.max_self_nanoseconds)},
            {"deadline_misses",count(stats.deadline_misses)},{"self_histogram",histogram},
            {"calls_hz",rate.calls_hz},{"produced_hz",rate.produced_hz},{"active",rate.calls!=0},
            {"errors_delta",count(rate.errors)},{"busy_fraction",rate.busy_fraction}});
        for(const auto& port:node.inputs)if(const auto input=executor.mixer_input_stats(node.id,port.id))
            mixers.append(QJsonObject{{"node",node.id},{"port",int(port.id)},{"depth",count(input->depth)},
                {"capacity",count(input->capacity)},{"dropped_samples",count(input->dropped_samples)},
                {"underrun_samples",count(input->underrun_samples)}});
    }
    void collect_node_statistics(const domain::ConfiguredProcessingNode& node,const dataflow_runtime::NodeStats& stats,RateSummary& summary,QJsonArray& nodes,QJsonArray& mixers) {
                const auto rate=calculate_node_rate(node.id,stats,summary);
                const auto [priority,basis]=processing_rate_role(node.kind);
                if(priority>summary.priority || (priority==summary.priority && priority && rate.produced_hz>summary.hz)){
                    summary.priority=priority;summary.hz=rate.produced_hz;summary.node=node.id;summary.basis=basis;}
                rate_baselines[node.id]=stats;
                append_node_statistics(node,stats,rate,nodes,mixers);
    }
    RateSummary collect_execution_statistics(std::uint64_t now,QJsonArray& nodes,QJsonArray& mixers) {
        if(rate_graph_revision!=state.graph_revision){rate_graph_revision=state.graph_revision;rate_sample_ns=0;rate_baselines.clear();}
        RateSummary summary;summary.measured=rate_sample_ns && now>rate_sample_ns;
        summary.seconds=summary.measured?double(now-rate_sample_ns)/1000000000.:0.;
        if(executor.plan())for(const auto& node:executor.plan()->numeric_definition()->nodes)
            if(const auto stats=executor.node_stats(node.id)) {
                collect_node_statistics(node,*stats,summary,nodes,mixers);
            }
        rate_sample_ns=now;
        return summary;
    }
    void append_clock_diagnostics(QJsonArray& clocks,QJsonArray& external) const {
        const auto count=[](std::uint64_t value){return QString::number(value);};
        for(const auto& [id,clock]:ingress.clocks)clocks.append(QJsonObject{{"node",id},{"sample_rate",int(clock.rate)},
            {"lag_samples",count(clock.lag_samples)},{"max_lag_samples",count(clock.max_lag_samples)},
            {"catchup_limits",count(clock.catchup_limits)}});
        for(const auto& [node,input]:ingress.external_inputs)
            external.append(QJsonObject{{"node",node},{"missing",input.missing},{"zero_frames",count(input.zero_frames)}});
    }
    static QJsonObject packet_queue_diagnostics(const SourceIngress::CaptureInput& input) {
        const auto count=[](std::uint64_t value){return QString::number(value);};
        const auto queue=input.packets.stats();
        return QJsonObject{{"node",input.node},{"port",int(input.port)},{"sample_rate",int(input.contract.clock.sample_rate)},
            {"queued_samples",count(queue.queued_samples)},{"capacity_samples",count(queue.capacity_samples)},
            {"queued_packets",count(queue.queued_packets)},{"capacity_packets",count(queue.capacity_packets)},
            {"accepted_samples",count(queue.accepted_samples)},{"overflow_packets",count(queue.overflow_packets)},
            {"dropped_samples",count(queue.dropped_samples+input.dropped_samples)},
            {"discontinuities",count(queue.discontinuities)},
            {"workspace_bytes",count(input.packets.workspace_bytes())}};
    }
    QJsonObject delivery_queue_diagnostics() const {
        const auto count=[](std::uint64_t value){return QString::number(value);};
        return {{"transport_frames",int(delivery.frames.size())},{"transport_rows",int(delivery.rows.size())},
            {"preview_frames",int(delivery.previews.size())},{"preview_rows",int(delivery.preview_rows.size())},
            {"transport_frame_drops",count(delivery.transport_frame_drops)},
            {"transport_row_drops",count(delivery.transport_row_drops)},
            {"preview_coalesced",count(delivery.preview_coalesced)},
            {"preview_row_drops",count(delivery.preview_row_drops)}};
    }
    void update_runtime_diagnostics(std::uint64_t now,const QJsonArray& source_diagnostics) {
            QJsonArray nodes,mixers,external,clock_diagnostics,pcm_packets;
            const auto rate=collect_execution_statistics(now,nodes,mixers);
            append_clock_diagnostics(clock_diagnostics,external);
            for(const auto& input:ingress.inputs)pcm_packets.append(packet_queue_diagnostics(input));
            state.diagnostics={{"pcm_packet_queues",pcm_packets},{"node_stats",nodes},{"sources",source_diagnostics},{"mixer_inputs",mixers},{"external_inputs",external},
                {"sample_clocks",clock_diagnostics},{"processing_measured",rate.measured && rate.node>=0},
                {"processing_hz",rate.hz},{"processing_rate_node",rate.node},{"processing_rate_basis",rate.basis},{"measurement_seconds",rate.seconds},
                {"queues",delivery_queue_diagnostics()}};
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
    if(!check || impl_->control_reply_checks.size()>=remote_numeric::ControlMaximumPeers)return 0;
    if(const auto* working=impl_->runtime.working_numeric_graph();working && !check(*working))return 0;
    const auto token=++impl_->next_reply_check;impl_->control_reply_checks.emplace(token,std::move(check));return token;
}
void NumericEngine::unregister_control_reply_check(std::uint64_t token){impl_->control_reply_checks.erase(token);}
application::ProcessingCapabilities NumericEngine::capabilities() const {return impl_->caps;}
domain::ProcessingDiagnostic NumericEngine::validate_numeric_node(const domain::ConfiguredProcessingNode& node) const {
    // The immutable profile and pure capability query do not access execution state.
    // Callers retain the engine lifetime; DSP can continue on its worker concurrently.
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
    const auto wait_start=(core::perf::enabled.load(std::memory_order_relaxed) || core::telemetry::enabled())?core::perf::now():0;
    std::lock_guard lock(impl_->mutex);
    if(wait_start)core::perf::record(core::perf::Stage::NumericQueryWait,wait_start,core::perf::now()-wait_start);
    core::perf::Scope hold(core::perf::Stage::NumericQueryHold);
    const auto context=impl_->delivery.contexts.find(endpoint);
    if(context==impl_->delivery.contexts.end() || !impl_->executor.plan() || !impl_->executor.context())return Status::unavailable;
    const auto history=impl_->delivery.history_ordinals.find(endpoint);
    if(history==impl_->delivery.history_ordinals.end())return Status::unsupported_version;
    return impl_->query_history(endpoint,context->second,out,ordinal,filled_count,is_history);
}
quint64 NumericEngine::submit(EngineCommand request) {
    const auto id=++impl_->public_request;EngineReply reply;reply.request_id=id;bool reconcile=false;
    QString adapter_error;
    bool adapter_valid=request.kind!=EngineCommand::ApplyGraph || !adapter_handlers_.validate_graph ||
        adapter_handlers_.validate_graph(request.graph,&adapter_error);
    std::map<domain::NodeId,QString> capture_devices;
    if(request.kind==EngineCommand::ApplyGraph && adapter_valid)
        adapter_valid=impl_->collect_capture_devices(request.graph,capture_devices,adapter_error);
    impl_->dispatch_public_command(request,reply,reconcile,std::move(capture_devices),adapter_valid,adapter_error);
    if(request.kind==EngineCommand::ApplyGraph && reply.accepted && adapter_handlers_.commit_graph)
        adapter_handlers_.commit_graph(request.graph);
    if(reconcile)impl_->ingress.reconcile_sources();
    impl_->queue_public_reply(std::move(reply));
    return id;
}
}

