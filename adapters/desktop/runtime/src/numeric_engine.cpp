#include <pipeline/numeric_engine.h>
#include <desktop/numeric_project_document.h>
#include <control/numeric_remote_support.h>
#include <dataflow_runtime/runtime.h>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
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

namespace pipeline {
namespace {
using namespace signal_contracts;
constexpr FrameLimits frame_limits{8,32768,262144,16384,256,8192};
dataflow_runtime::NumericRuntimeProfile desktop_profile() {
    // Process admission charges bounded delivery and control ownership ceilings
    // separately from the graph's plan/payload/scratch. These are reservations,
    // not an assertion that each ceiling is allocated at startup.
    constexpr std::size_t fixed=256*(262144+16384)+(384+48)*1024*1024;
    return {{512,2048,8,32,256,4*1024*1024,frame_limits},
        {{16*1024*1024,32*1024*1024,16*1024*1024,1024*1024},fixed,576*1024*1024},
        {32768,8,262144,262144,16*1024*1024},{true,true,true},true};
}
struct DeliveryArena {
    static constexpr std::size_t slot_count=128,payload_bytes=262144,metadata_bytes=16384;
    std::vector<std::byte> payload{slot_count*payload_bytes},metadata{slot_count*metadata_bytes};
    std::array<BufferSlot,slot_count> payload_slots{},metadata_slots{};
    BufferPool payload_pool{payload,payload_slots,payload_bytes};
    BufferPool metadata_pool{metadata,metadata_slots,metadata_bytes};
};
QString failure(Status status) {return QStringLiteral("Numeric processing refused (%1)").arg(int(status));}
}

struct NumericEngine::Impl final : application::NumericGraphPreparationPort,application::GraphRuntimePort, application::ControlClock {
    NumericEngine& owner;
    AudioSourceFactory factory;
    dataflow_runtime::NumericRuntimeProfile profile=desktop_profile();
    dataflow_runtime::GraphExecutor executor{profile};
    static bool physical_supported(const domain::ResolvedProcessingNode& node) noexcept {
        if(node.kind!="AudioSource" && node.kind!="TestSignal")return true;
        if(node.outputs.size()!=1 || !node.outputs[0].contract)return false;
        const auto data=node.outputs[0].contract.view();const auto* c=std::get_if<NumericContract>(&data);
        if(!c || c->dtype!=DType::Float32 ||
            c->representation.encoding!=StorageEncoding::Native || c->shape.rank!=Rank::Vector ||
            c->shape.axes[0]!=Axis::Sample || c->channel_order.empty() || c->channel_order.size()>2 ||
            (!c->clock.sample_rate || c->clock.sample_rate>192000) || c->clock.tick_seconds.numerator!=1 ||
            c->clock.tick_seconds.denominator!=c->clock.sample_rate || c->shape.extents[0].maximum>8192 ||
            c->coordinates[0].mode!=CoordinateMode::None)return false;
        {
            domain::ProcessingParameterView rate;
            if(domain::resolve_processing_parameter(node,"sample_rate",rate)!=Status::ok)return false;
            const auto* value=std::get_if<NumericLiteral>(&rate);
            if(!value || std::visit([](auto v){return double(v);},*value)!=c->clock.sample_rate)return false;
        }
        return std::all_of(c->channel_order.begin(),c->channel_order.end(),[](auto id){return id<2;});
    }
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
            if(!physical_supported(node) || !transport_supported(node))return domain::ProcessingSupport{};
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
    std::shared_ptr<DeliveryArena> arena=std::make_shared<DeliveryArena>(),preview_arena=std::make_shared<DeliveryArena>();
    std::shared_ptr<const void> retention_arenas=std::make_shared<std::pair<std::shared_ptr<DeliveryArena>,std::shared_ptr<DeliveryArena>>>(arena,preview_arena);
    std::map<EndpointIdentity,std::shared_ptr<const NumericDeliveryContext>> contexts;
    std::map<EndpointIdentity,std::uint64_t> history_ordinals;
    std::map<EndpointIdentity,DeliveredNumericFrame> latest_matrices; // Prepared keys; shares bounded retained delivery ownership.
    std::map<EndpointIdentity,bool> delivery_gaps;
    struct WledOutput {
        QHostAddress destination,interface;quint16 port=11988;
        desktop::NumericWledOutputPolicy policy;
        std::shared_ptr<QUdpSocket> socket;
    };
    std::map<domain::NodeId,WledOutput> wled_outputs;
    struct CaptureInput {
        domain::NodeId node=-1;
        AudioSourceKind source=AudioSourceKind::SystemOutput;
        QJsonObject configuration;
        domain::OwnedProcessingContract owned;
        NumericContract contract;
        std::array<PlaneLayout,2> planes{};
        std::vector<std::byte> bytes;
        std::size_t block=0,filled=0;
        std::int64_t ticks=0;
        std::uint64_t sequence=0,generation=0;
        std::uint64_t producer_epoch=0;
        bool gap=true;
        bool admission_failed=false;
    };
    std::vector<CaptureInput> inputs;
    std::map<domain::NodeId,QString> capture_devices;
    struct ActiveSource {AudioSourceKind kind;QJsonObject configuration;std::unique_ptr<IAudioSource> source;};
    std::map<domain::NodeId,ActiveSource> sources;
    std::map<domain::NodeId,std::uint64_t> source_tokens;
    std::map<domain::NodeId,ComponentStatus> source_states;
    NumericDeliveryMailbox<DeliveredNumericFrame,128> frames,previews;
    NumericDeliveryMailbox<DeliveredNumericHistoryRow,128> rows;
    std::vector<EndpointIdentity> subscriptions;
    struct Tap {
        std::shared_ptr<std::atomic_bool> active=std::make_shared<std::atomic_bool>(true);
        std::vector<EndpointIdentity> endpoints;
        std::function<void(DeliveredNumericFrame)> frame;
        std::function<void(DeliveredNumericHistoryRow)> history;
    };
    std::map<std::uint64_t,Tap> taps;
    std::uint64_t next_tap=0;
    mutable std::mutex mutex;
    std::mutex control_mutex;
    std::uint64_t next_reply_check=0;
    std::map<std::uint64_t,std::function<bool(const domain::NumericProcessingGraph&)>> control_reply_checks;
    bool retained_replies_fit(const domain::NumericProcessingGraph& graph) const {
        for(const auto& [id,check]:control_reply_checks)if(!check(graph))return false;return true;
    }
    std::unique_lock<std::mutex> pending_commit;
    EngineSnapshot state;
    bool capture_requested=false;
    std::uint64_t control_request=0,public_request=0,capture_fence=1;
    QTimer timer;
    QTimer wled_timer;
    std::function<void(EngineSnapshot)> snapshot_handler;
    std::function<void(EngineReply)> reply_handler;
    std::function<void(DeliveredNumericFrame)> frame_handler;
    std::function<void(DeliveredNumericHistoryRow)> history_handler;
    QString network_error;
    std::uint64_t snapshot_generation=0,reply_generation=0,delivery_generation=0;
    std::shared_ptr<std::atomic_bool> frame_active=std::make_shared<std::atomic_bool>(true),history_active=std::make_shared<std::atomic_bool>(true);

    Impl(NumericEngine& target,AudioSourceFactory source):owner(target),factory(std::move(source)) {
        if(!factory)factory=default_audio_source_factory();
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
        QObject::connect(&timer,&QTimer::timeout,&owner,[this]{drain();});timer.start(16);
        wled_timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&wled_timer,&QTimer::timeout,&owner,[this]{flush_wled();});
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
                    if(const auto device=capture_devices.find(node.value("id").toInt());device!=capture_devices.end()) {
                        node["adapter"]=QJsonObject{{"device_identity",device->second}};nodes[i]=node;
                    }
                }
                state.graph["nodes"]=nodes;
            }
        }
        state.statuses[QStringLiteral("analyzer")]={reply.state.run_requested?State::Running:State::Stopped,{},state.revision};
    }
    Status prepare_numeric(const domain::NumericProcessingGraph& graph,const application::NumericRuntimeLimits& limits,
        application::PreparedExecution& out) override {
        for(const auto& node:graph.nodes) {
            domain::ResolvedProcessingNode resolved;
            if(!domain::resolve_processing_node(node,profile.admission,resolved) || !physical_supported(resolved) || !transport_supported(resolved))return Status::incompatible_contract;
        }
        remote_numeric::CapabilitySnapshot candidate_capabilities;
        if(candidate_capabilities.assign(graph,caps)!=Status::ok ||
           !control::numeric::reply_envelope_fits(graph,remote_numeric::control_limits(),candidate_capabilities.value))return Status::exhausted;
        if(!retained_replies_fit(graph))return Status::exhausted;
        dataflow_runtime::GraphExecutor::PreparationBase base;
        {std::lock_guard lock(mutex);base=executor.snapshot_preparation_base();}
        const auto status=executor.prepare_numeric_detached(graph,limits,base,out);
        if(status!=Status::ok)return status;
        {std::lock_guard lock(mutex);return executor.register_prepared(out,base);}
    }
    Status apply(application::PreparedExecution&& prepared) override {
        if(!prepared.plan || !prepared.plan->numeric_definition())return Status::invalid_argument;
        // A peer can arrive after Prepare; recheck every pinned reply envelope before committing.
        if(!retained_replies_fit(*prepared.plan->numeric_definition()))return Status::exhausted;
        std::unique_lock lock(mutex);
        try {
        if(!prepared.plan || !prepared.plan->numeric_definition())return Status::invalid_argument;
        const auto& graph=*prepared.plan->numeric_definition();
        decltype(contexts) candidate;
        decltype(history_ordinals) ordinals;
        decltype(latest_matrices) latest;
        decltype(delivery_gaps) gaps;
        decltype(wled_outputs) wled;
        std::vector<CaptureInput> capture;
        for(const auto& node:graph.nodes) {
            domain::ResolvedProcessingNode resolved;
            if(!domain::resolve_processing_node(node,profile.admission,resolved))return Status::invalid_argument;
            const auto add=[&](const domain::ProcessingPort& port) {
                auto context=std::make_shared<NumericDeliveryContext>();
                context->endpoint={graph.graph_id,node.id,port.direction,port.id};
                context->contract=port.contract;context->retention_owner=retention_arenas;
                const auto data=port.contract.view();
                if(const auto* numeric=std::get_if<NumericContract>(&data);numeric && numeric->shape.rank==Rank::Matrix) {
                    const auto prior=history_ordinals.find(context->endpoint);
                    const auto old=contexts.find(context->endpoint);
                    const bool compatible=old!=contexts.end() && same_contract(old->second->contract.view(),data);
                    ordinals.emplace(context->endpoint,!compatible || prior==history_ordinals.end()?0:prior->second);
                    auto cached=latest_matrices.find(context->endpoint);DeliveredNumericFrame retained;
                    if(compatible && cached!=latest_matrices.end()){retained=cached->second;retained.context=context;}
                    latest.emplace(context->endpoint,std::move(retained));
                }
                const auto pending=delivery_gaps.find(context->endpoint);
                const auto old_context=contexts.find(context->endpoint);
                gaps.emplace(context->endpoint,pending!=delivery_gaps.end()&&pending->second&&old_context!=contexts.end()&&same_contract(old_context->second->contract.view(),data));
                candidate.emplace(context->endpoint,std::move(context));
            };
            for(const auto& port:node.outputs)add(port);
            if(resolved.role==domain::NodeRole::Sink)for(const auto& port:node.inputs)add(port);
            if(node.kind=="WLEDOutput" && node.enabled) {
                WledOutput output;std::uint32_t cadence=17;
                for(const auto key:{"destination","interface","port","cadence_ms"}) {
                    domain::ProcessingParameterView value;
                    if(domain::resolve_processing_parameter(resolved,key,value)!=Status::ok)return Status::invalid_argument;
                    if(const auto* text=std::get_if<std::string_view>(&value)) {
                        QHostAddress address(QString::fromUtf8(text->data(),int(text->size())));
                        if(address.isNull())return Status::invalid_argument;
                        if(std::string_view(key)=="destination")output.destination=address;else output.interface=address;
                    } else if(const auto* number=std::get_if<NumericLiteral>(&value)) {
                        const auto n=std::visit([](auto x){return std::uint32_t(x);},*number);
                        if(std::string_view(key)=="port")output.port=quint16(n);else cadence=n;
                    } else return Status::invalid_argument;
                }
                output.policy.set_cadence(cadence);
                const auto old=wled_outputs.find(node.id);
                if(old!=wled_outputs.end() && old->second.destination==output.destination && old->second.interface==output.interface && old->second.port==output.port) {
                    output=old->second;output.policy.set_cadence(cadence);
                } else output.socket=std::make_shared<QUdpSocket>();
                if(!output.policy.running())output.policy.start(now_ms());
                wled.emplace(node.id,std::move(output));
            }
            if(node.kind!="AudioSource" && node.kind!="TestSignal")continue;
            if(node.outputs.size()!=1 || !node.outputs[0].contract)return Status::invalid_argument;
            CaptureInput input;input.node=node.id;input.owned=node.outputs[0].contract;
            const auto contract=input.owned.view();const auto* numeric=std::get_if<NumericContract>(&contract);
            if(!numeric || numeric->dtype!=DType::Float32 ||
                numeric->representation.encoding!=StorageEncoding::Native || numeric->shape.rank!=Rank::Vector ||
                numeric->shape.axes[0]!=Axis::Sample || numeric->channel_order.empty() || numeric->channel_order.size()>2 ||
                !numeric->clock.sample_rate || numeric->clock.tick_seconds.numerator!=1 ||
                numeric->clock.tick_seconds.denominator!=numeric->clock.sample_rate ||
                numeric->clock.sample_rate>768000 || numeric->shape.extents[0].maximum>8192)
                return Status::incompatible_contract;
            for(auto channel:numeric->channel_order)if(channel>1)return Status::incompatible_contract;
            if(numeric->coordinates[0].mode!=CoordinateMode::None)return Status::incompatible_contract;
            input.contract=*numeric;input.block=std::size_t(numeric->shape.extents[0].maximum);
            if(!input.block)return Status::invalid_argument;
            input.source=node.kind=="TestSignal"?AudioSourceKind::TestSignal:AudioSourceKind::SystemOutput;
            input.configuration["sample_rate"]=int(numeric->clock.sample_rate);
            if(node.kind=="AudioSource")if(const auto device=capture_devices.find(node.id);device!=capture_devices.end())
                input.configuration["device_id"]=device->second;
            if(node.kind=="TestSignal")for(const auto key:{"tones","minimum_hz","maximum_hz","amplitude","period_seconds","sweep_depth"}) {
                domain::ProcessingParameterView value;
                if(domain::resolve_processing_parameter(resolved,key,value)!=Status::ok)return Status::invalid_argument;
                const auto* literal=std::get_if<NumericLiteral>(&value);if(!literal)return Status::invalid_argument;
                input.configuration[key]=std::visit([](auto v){return double(v);},*literal);
            }
            if(const auto* params=std::get_if<domain::EndpointProcessingParameters>(&node.parameters))
                for(const auto& p:params->values)if(p.key=="source") {
                    const auto* literal=std::get_if<NumericLiteral>(&p.value);
                    if(!literal)return Status::invalid_argument;
                    const auto selected=std::visit([](auto x){return double(x);},*literal);
                    if(selected!=0 && selected!=1 && selected!=2)return Status::invalid_argument;
                    input.source=AudioSourceKind(int(selected));
                }
            const auto channels=input.contract.channel_order.size();
            input.bytes.resize(input.block*channels*sizeof(float));
            for(std::size_t i=0;i<channels;++i)input.planes[i]=
                input.contract.representation.layout==ChannelLayout::Planar
                ?PlaneLayout{i*input.block*sizeof(float),{sizeof(float),0}}
                :PlaneLayout{i*sizeof(float),{channels*sizeof(float),0}};
            const auto prior=std::find_if(inputs.begin(),inputs.end(),[&](const auto& p) {
                return p.node==input.node && p.source==input.source &&
                    (p.configuration==input.configuration || input.source==AudioSourceKind::TestSignal) && same_contract(p.owned.view(),input.owned.view());
            });
            if(prior!=inputs.end()){auto configuration=input.configuration;input=*prior;input.configuration=std::move(configuration);}
            else if(const auto token=source_tokens.find(input.node);token!=source_tokens.end())input.producer_epoch=state.epoch+token->second;
            capture.push_back(std::move(input));
        }
        const auto status=executor.apply(std::move(prepared));
        if(status!=Status::ok)return status;
        contexts.swap(candidate);history_ordinals.swap(ordinals);latest_matrices.swap(latest);delivery_gaps.swap(gaps);inputs.swap(capture);wled_outputs.swap(wled);
        if(!wled_outputs.empty()) {
            auto cadence=std::uint32_t(60000);for(const auto& [node,output]:wled_outputs)cadence=std::min(cadence,output.policy.cadence_ms());
            wled_timer.setInterval(int(cadence));wled_timer.start();
        } else wled_timer.stop();
        ++delivery_generation;frames.clear();previews.clear();rows.clear();pending_commit=std::move(lock);return Status::ok;
        } catch(const std::bad_alloc&) {return Status::exhausted;}
    }
    Status shutdown() override {
        timer.stop();stop_sources();
        std::lock_guard lock(mutex);++delivery_generation;
        const auto status=executor.shutdown();capture_requested=false;wled_timer.stop();
        frames.clear();previews.clear();rows.clear();wled_outputs.clear();inputs.clear();contexts.clear();history_ordinals.clear();latest_matrices.clear();delivery_gaps.clear();
        return status;
    }
    bool tap_selected(const EndpointIdentity& endpoint) const {
        for(const auto& [id,tap]:taps)if(tap.endpoints.empty() ||
            std::find(tap.endpoints.begin(),tap.endpoints.end(),endpoint)!=tap.endpoints.end())return true;
        return false;
    }
    bool selected(const EndpointIdentity& endpoint) const {
        return tap_selected(endpoint)||std::find(subscriptions.begin(),subscriptions.end(),endpoint)!=subscriptions.end();
    }
    void output(const dataflow_runtime::NumericPortFrameView& borrowed) {
        // Contexts are prepared: lookup avoids constructing/copying an identity on the hot path.
        auto found=std::find_if(contexts.begin(),contexts.end(),[&](const auto& entry) {
            const auto& key=entry.first;return key.node==borrowed.identity.node && key.port==borrowed.identity.port &&
                key.direction==borrowed.direction && key.graph_id==borrowed.graph_id;
        });
        if(found==contexts.end() || (!selected(found->first) &&
            !(borrowed.direction==domain::PortDirection::Input && wled_outputs.contains(borrowed.identity.node))))return;
        // Prepared History endpoints publish compact row/lifecycle events through history().
        // A second generic delivery would reject the borrowed matrix and reset consumers.
        // Explicit query_numeric remains the owning full-history snapshot boundary.
        if(const auto* numeric=std::get_if<NumericView>(&borrowed.frame.payload);numeric && numeric->ring.capacity)return;
        auto cached=latest_matrices.find(found->first);
        // Release the prior shared slot before retaining its replacement. Pool
        // exhaustion stays bounded and cannot permanently pin all prior values.
        if(cached!=latest_matrices.end())cached->second={};
        if(borrowed.direction==domain::PortDirection::Input)if(auto target=wled_outputs.find(borrowed.identity.node);target!=wled_outputs.end()) {
            if(borrowed.frame.stamp.available)target->second.policy.update(borrowed.frame,now_ms());else target->second.policy.unavailable();
        }
        if(std::find(subscriptions.begin(),subscriptions.end(),found->first)!=subscriptions.end()) {
            const auto count=previews.size();for(std::size_t i=0;i<count;++i){auto old=previews.pop();if(old&&old->context&&old->context->endpoint!=found->first)previews.push(std::move(*old));}
            if(previews.size()==128)previews.pop();
            DeliveredNumericFrame preview;
            const auto status=retain_numeric_delivery(found->second,{borrowed.delivery.graph_revision,borrowed.delivery.epoch},borrowed.frame,borrowed.status,
                frame_limits,preview_arena->payload_pool,preview_arena->metadata_pool,preview);
            if(status!=Status::ok)preview={found->second,{borrowed.delivery.graph_revision,borrowed.delivery.epoch},{},status};
            if(cached!=latest_matrices.end())cached->second=preview;
            previews.push(std::move(preview));
        }
        if(!tap_selected(found->first))return;
        // Loss-sensitive processing/transport delivery is FIFO. GUI previews
        // coalesce downstream, after this bounded owning boundary.
        auto pending_gap=delivery_gaps.find(found->first);
        if(frames.size()==128){if(pending_gap!=delivery_gaps.end())pending_gap->second=true;return;}
        auto delivered_frame=borrowed.frame;
        // Taps include network consumers: report skipped frames at this
        // adapter boundary without altering the prepared graph's DSP history.
        delivered_frame.stamp.discontinuity|=(pending_gap!=delivery_gaps.end()&&pending_gap->second);
        DeliveredNumericFrame retained;
        const auto status=retain_numeric_delivery(found->second,{borrowed.delivery.graph_revision,borrowed.delivery.epoch},
            delivered_frame,borrowed.status,frame_limits,arena->payload_pool,arena->metadata_pool,retained);
        if(pending_gap!=delivery_gaps.end())pending_gap->second=status!=Status::ok;
        if(status!=Status::ok)retained={found->second,{borrowed.delivery.graph_revision,borrowed.delivery.epoch},{},status};
        if(cached!=latest_matrices.end())cached->second=retained;
        if(!frames.push(std::move(retained))) {
            if(pending_gap!=delivery_gaps.end())pending_gap->second=true;
        }
    }
    void history(const dataflow_runtime::NumericHistoryRowView& borrowed) {
        const auto& endpoint=borrowed.endpoint;
        auto found=std::find_if(contexts.begin(),contexts.end(),[&](const auto& entry) {
            const auto& key=entry.first;return key.node==endpoint.identity.node && key.port==endpoint.identity.port &&
                key.direction==endpoint.direction && key.graph_id==endpoint.graph_id;
        });
        if(found==contexts.end())return;
        if(auto ordinal=history_ordinals.find(found->first);ordinal!=history_ordinals.end())ordinal->second=borrowed.accepted_row_ordinal;
        if(!selected(found->first))return;
        NumericHistoryEvent event=NumericHistoryEvent::Unavailable;
        switch(borrowed.event) {
        case dataflow_runtime::NumericHistoryEventKind::Row:event=NumericHistoryEvent::Row;break;
        case dataflow_runtime::NumericHistoryEventKind::Empty:event=NumericHistoryEvent::Empty;break;
        case dataflow_runtime::NumericHistoryEventKind::Reset:event=NumericHistoryEvent::Reset;break;
        case dataflow_runtime::NumericHistoryEventKind::Unavailable:break;
        }
        DeliveredNumericHistoryRow retained;
        const auto status=retain_numeric_history_row(found->second,{endpoint.delivery.graph_revision,endpoint.delivery.epoch},event,
            borrowed.compact,borrowed.filled_count,borrowed.capacity,borrowed.accepted_row_ordinal,frame_limits,
            arena->payload_pool,arena->metadata_pool,retained);
        if(status!=Status::ok){retained={};retained.event=NumericHistoryEvent::Unavailable;
            retained.compact={found->second,{endpoint.delivery.graph_revision,endpoint.delivery.epoch},{},status};
            retained.capacity=borrowed.capacity;retained.filled_count=borrowed.filled_count;}
        if(!rows.push(std::move(retained))) {
            rows.pop();DeliveredNumericHistoryRow marker;marker.event=NumericHistoryEvent::Unavailable;
            marker.compact={found->second,{endpoint.delivery.graph_revision,endpoint.delivery.epoch},{},Status::exhausted};
            marker.capacity=borrowed.capacity;rows.push(std::move(marker));
        }
    }
    Status process(domain::NodeId node,const DataFrameView& frame) {
        return executor.process_numeric_ports(node,frame,{state.graph_revision,state.epoch},
            {[](void* p,const auto& f){static_cast<Impl*>(p)->output(f);},
             [](void* p,const auto& f){static_cast<Impl*>(p)->output(f);},
             [](void* p,const auto& f){static_cast<Impl*>(p)->history(f);},this});
    }
    void capture(domain::NodeId node,std::uint64_t fence,const AudioFrame& frame) {
        std::lock_guard lock(mutex);
        const auto token=source_tokens.find(node);
        if(token==source_tokens.end() || fence!=token->second || !executor.running())return;
        for(auto& input:inputs)if(input.node==node) {
            const auto unavailable=[&] {
                input.filled=0;input.gap=true;FrameStamp stamp;
                stamp.sequence=++input.sequence;stamp.generation=input.generation;stamp.available=false;
                stamp.producer_epoch=input.producer_epoch;stamp.epoch=state.epoch;stamp.graph_revision=state.graph_revision;
                NumericView numeric;numeric.contract=&input.contract;process(input.node,{stamp,numeric});
            };
            const bool needs_right=std::find(input.contract.channel_order.begin(),input.contract.channel_order.end(),1)!=input.contract.channel_order.end();
            if(frame.sample_rate!=input.contract.clock.sample_rate || (needs_right && frame.left.size()!=frame.right.size()) ||
                frame.timestamp_ns<0){
                if(!input.admission_failed) {
                    input.admission_failed=true;
                    const auto error=QStringLiteral("Audio capture contract mismatch: expected %1 Hz, stereo=%2; received %3 Hz, left=%4, right=%5, timestamp=%6")
                        .arg(input.contract.clock.sample_rate).arg(needs_right).arg(frame.sample_rate)
                        .arg(frame.left.size()).arg(frame.right.size()).arg(frame.timestamp_ns);
                    QMetaObject::invokeMethod(&owner,[this,node,fence,error]{source_status(node,fence,false,error);},Qt::QueuedConnection);
                }
                unavailable();continue;
            }
            if(input.admission_failed) {
                input.admission_failed=false;
                QMetaObject::invokeMethod(&owner,[this,node,fence]{source_status(node,fence,true,{});},Qt::QueuedConnection);
            }
            if(frame.discontinuity || frame.generation!=input.generation){input.filled=0;input.gap=true;}
            input.generation=frame.generation;
            for(std::size_t sample=0;sample<frame.left.size();++sample) {
                if(!input.filled) {
                    const auto rate=std::int64_t(frame.sample_rate);
                    input.ticks=(frame.timestamp_ns/1000000000)*rate+
                        (frame.timestamp_ns%1000000000)*rate/1000000000+std::int64_t(sample);
                }
                for(std::size_t plane=0;plane<input.contract.channel_order.size();++plane) {
                    const float value=input.contract.channel_order[plane]==0?frame.left[sample]:frame.right[sample];
                    const auto offset=input.planes[plane].offset+input.filled*input.planes[plane].stride_bytes[0];
                    std::memcpy(input.bytes.data()+offset,&value,sizeof(value));
                }
                if(++input.filled!=input.block)continue;
                NumericView numeric{&input.contract,input.bytes,
                    std::span(input.planes).first(input.contract.channel_order.size()),{input.block,1}};
                FrameStamp stamp;stamp.sequence=stamp.source_sequence=++input.sequence;stamp.generation=input.generation;
                stamp.epoch=state.epoch;stamp.producer_epoch=input.producer_epoch;stamp.graph_revision=state.graph_revision;
                stamp.timestamp_ticks=input.ticks;stamp.discontinuity=input.gap;
                const auto result=process(input.node,{stamp,numeric});input.filled=0;
                if(result==Status::ok)input.gap=false;
                else if(result!=Status::would_block)unavailable();
            }
        }
    }
    void stop_sources(){
        {std::lock_guard lock(mutex);source_tokens.clear();source_states.clear();}
        for(auto& [node,source]:sources)source.source->stop();sources.clear();
    }
    void update_capture_status() {
        ComponentStatus aggregate{State::Stopped,{},state.revision};
        if(capture_requested && !inputs.empty()) {
            aggregate.state=State::Running;
            for(const auto& input:inputs) {
                const auto found=source_states.find(input.node);
                if(found!=source_states.end() && found->second.state==State::Failed) {
                    aggregate.state=State::Failed;
                    if(!aggregate.error.isEmpty())aggregate.error+='\n';
                    aggregate.error+=QStringLiteral("%1: %2").arg(input.node).arg(found->second.error);
                } else if(aggregate.state!=State::Failed &&
                    (found==source_states.end() || found->second.state!=State::Running))aggregate.state=State::Starting;
            }
        }
        state.statuses[QStringLiteral("capture")]=std::move(aggregate);
    }
    void source_status(domain::NodeId node,std::uint64_t fence,bool running,const QString& error) {
        {std::lock_guard lock(mutex);const auto token=source_tokens.find(node);if(token==source_tokens.end() || token->second!=fence)return;
            source_states[node]={running?State::Running:State::Failed,error,state.revision};
            update_capture_status();
            if(!running)for(auto& input:inputs)if(input.node==node) {
                input.filled=0;input.gap=true;FrameStamp stamp;
                stamp.sequence=++input.sequence;stamp.generation=input.generation;stamp.available=false;
                stamp.producer_epoch=input.producer_epoch;stamp.epoch=state.epoch;stamp.graph_revision=state.graph_revision;
                NumericView numeric;numeric.contract=&input.contract;process(input.node,{stamp,numeric});
            }
        }
        if(snapshot_handler)snapshot_handler(owner.snapshot());
    }
    void reconcile_sources() {
        std::map<domain::NodeId,std::pair<AudioSourceKind,QJsonObject>> needed;
        {std::lock_guard lock(mutex);
            if(capture_requested)for(const auto& input:inputs)needed.emplace(input.node,std::pair{input.source,input.configuration});
            std::erase_if(source_tokens,[&](const auto& value){return !needed.contains(value.first);});
            std::erase_if(source_states,[&](const auto& value){return !needed.contains(value.first);});}
        for(auto it=sources.begin();it!=sources.end();) {
            const auto next=needed.find(it->first);
            if(next!=needed.end() && next->second.first==it->second.kind && next->second.second==it->second.configuration){++it;continue;}
            if(next!=needed.end() && next->second.first==it->second.kind &&
                it->second.source->reconfigure_live(next->second.second)) {
                // A replaced capture input restarts its sequence during prepare,
                // while a live source keeps the callback token. Give that new
                // capture stream an epoch newer than the retained executor input.
                // TestSignal edits retain the existing input counters instead.
                {std::lock_guard lock(mutex);
                    for(auto& input:inputs)if(input.node==it->first &&
                        (!input.producer_epoch || input.source!=AudioSourceKind::TestSignal)) {
                        input.producer_epoch=state.epoch+(++capture_fence);
                        input.filled=0;input.gap=true;
                    }
                }
                it->second.configuration=next->second.second;++it;continue;
            }
            {std::lock_guard lock(mutex);source_tokens.erase(it->first);source_states.erase(it->first);}
            it->second.source->stop();it=sources.erase(it);
        }
        for(const auto& [node,settings]:needed) {
            if(sources.contains(node))continue;
            const auto kind=settings.first;
            auto source=factory(kind);if(!source){
                std::uint64_t fence;
                {std::lock_guard lock(mutex);fence=++capture_fence;source_tokens[node]=fence;
                    for(auto& input:inputs)if(input.node==node){input.producer_epoch=state.epoch+fence;input.filled=0;}}
                source_status(node,fence,false,QStringLiteral("Capture source unavailable"));continue;
            }
            source->configure(settings.second);
            std::uint64_t fence;
            {std::lock_guard lock(mutex);fence=++capture_fence;source_tokens[node]=fence;
                source_states[node]={State::Starting,{},state.revision};
                for(auto& input:inputs)if(input.node==node){input.producer_epoch=state.epoch+fence;input.filled=0;}}
            source->set_audio_handler([this,node,fence](AudioFrame frame){capture(node,fence,frame);});
            source->set_status_handler([this,node,fence](bool running,QString error){
                QMetaObject::invokeMethod(&owner,[this,node,fence,running,error=std::move(error)]{
                    source_status(node,fence,running,error);},Qt::QueuedConnection);});
            source->set_lifecycle_handler([this,node,fence](bool ready,std::uint64_t,QString error){
                QMetaObject::invokeMethod(&owner,[this,node,fence,ready,error=std::move(error)]{
                    source_status(node,fence,ready,error);},Qt::QueuedConnection);});
            source->start();sources.emplace(node,ActiveSource{kind,settings.second,std::move(source)});
        }
        reproject_failed_sources();
        {std::lock_guard lock(mutex);update_capture_status();}
    }
    void reproject_failed_sources() {
        // A compatible Apply or newly admitted preview demand can expose a
        // retained failed source without reopening it or receiving a callback.
        struct FailedSource {domain::NodeId node;std::uint64_t fence;QString error;};
        std::vector<FailedSource> failed;
        {std::lock_guard lock(mutex);for(const auto& [node,status]:source_states)
            if(capture_requested&&status.state==State::Failed)if(const auto token=source_tokens.find(node);token!=source_tokens.end())
                failed.push_back({node,token->second,status.error});}
        for(const auto& source:failed)source_status(source.node,source.fence,false,source.error);
    }
    void drain() {
        // Copies carry cancellation identity; unsubscribe invalidates already copied work.
        QPointer<NumericEngine> lifetime(&owner);
        for(std::size_t i=0;i<128;++i) {
            std::optional<DeliveredNumericFrame> next;std::function<void(DeliveredNumericFrame)> handler;
            std::shared_ptr<std::atomic_bool> active;std::uint64_t generation;
            {std::lock_guard lock(mutex);next=previews.pop();generation=delivery_generation;
                if(next && next->context && next->delivery.epoch==state.epoch && next->delivery.graph_revision==state.graph_revision &&
                   std::find(subscriptions.begin(),subscriptions.end(),next->context->endpoint)!=subscriptions.end()){handler=frame_handler;active=frame_active;}}
            if(!next)break;
            {std::lock_guard lock(mutex);if(generation!=delivery_generation || next->delivery.epoch!=state.epoch || next->delivery.graph_revision!=state.graph_revision)break;}
            if(handler && active->load(std::memory_order_acquire))handler(*next);
            if(!lifetime)return;
            {std::lock_guard lock(mutex);if(generation!=delivery_generation)break;}
        }

        for(std::size_t i=0;i<128;++i) {
            std::optional<DeliveredNumericFrame> next;
            struct Pending {std::function<void(DeliveredNumericFrame)> callback;std::shared_ptr<std::atomic_bool> active;};
            std::array<Pending,9> handlers;
            std::uint64_t generation=0;
            std::size_t count=0;
            {std::lock_guard lock(mutex);next=frames.pop();generation=delivery_generation;
                if(next && next->context && next->delivery.epoch==state.epoch && next->delivery.graph_revision==state.graph_revision) {
                    const auto& endpoint=next->context->endpoint;
                    for(const auto& [id,tap]:taps)if(tap.frame && (tap.endpoints.empty() || std::find(tap.endpoints.begin(),tap.endpoints.end(),endpoint)!=tap.endpoints.end()))handlers[count++]={tap.frame,tap.active};

                }
            }
            if(!next)break;
            for(std::size_t h=0;h<count;++h) {
                {std::lock_guard lock(mutex);if(generation!=delivery_generation || next->delivery.epoch!=state.epoch || next->delivery.graph_revision!=state.graph_revision)break;}
                if(handlers[h].active->load(std::memory_order_acquire))handlers[h].callback(*next);
                if(!lifetime)return;
            }
        }
        for(std::size_t i=0;i<128;++i) {
            std::optional<DeliveredNumericHistoryRow> next;
            struct Pending {std::function<void(DeliveredNumericHistoryRow)> callback;std::shared_ptr<std::atomic_bool> active;};
            std::array<Pending,9> handlers;
            std::uint64_t generation=0;
            std::size_t count=0;
            {std::lock_guard lock(mutex);next=rows.pop();generation=delivery_generation;
                if(next && next->compact.context && next->compact.delivery.epoch==state.epoch && next->compact.delivery.graph_revision==state.graph_revision) {
                    const auto& endpoint=next->compact.context->endpoint;
                    if(history_handler && std::find(subscriptions.begin(),subscriptions.end(),endpoint)!=subscriptions.end())handlers[count++]={history_handler,history_active};
                    for(const auto& [id,tap]:taps)if(tap.history && (tap.endpoints.empty() || std::find(tap.endpoints.begin(),tap.endpoints.end(),endpoint)!=tap.endpoints.end()))handlers[count++]={tap.history,tap.active};
                }
            }
            if(!next)break;
            for(std::size_t h=0;h<count;++h) {
                {std::lock_guard lock(mutex);if(generation!=delivery_generation || next->compact.delivery.epoch!=state.epoch || next->compact.delivery.graph_revision!=state.graph_revision)break;}
                if(handlers[h].active->load(std::memory_order_acquire))handlers[h].callback(*next);
                if(!lifetime)return;
            }
        }
    }
    void flush_wled() {
        struct Send {domain::NodeId node;std::shared_ptr<QUdpSocket> socket;QHostAddress destination,interface;quint16 port;wled_protocol::WledPacket packet;};
        std::array<Send,512> pending;std::size_t count=0;
        {std::lock_guard lock(mutex);for(auto& [node,output]:wled_outputs) {
            wled_protocol::WledPacket packet;if(output.policy.take_due_packet(now_ms(),packet)!=Status::ok)continue;
            pending[count++]={node,output.socket,output.destination,output.interface,output.port,packet};
        }}
        for(std::size_t i=0;i<count;++i) {
            auto& output=pending[i];const bool bound=output.socket->state()!=QAbstractSocket::UnconnectedState || output.socket->bind(output.interface,quint16(0));
            const bool sent=bound && output.socket->writeDatagram(reinterpret_cast<const char*>(output.packet.data()),output.packet.size(),output.destination,output.port)==qint64(output.packet.size());
            {std::lock_guard lock(mutex);const auto current=wled_outputs.find(output.node);
                if(current==wled_outputs.end() || current->second.socket!=output.socket)continue;
                current->second.policy.note_send_result(sent);
                if(!sent)state.statuses[QStringLiteral("wled")]={State::Failed,output.socket->errorString(),state.revision};
            }
        }
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
    {std::lock_guard lock(impl_->mutex);if(impl_->subscriptions==endpoints)return true;impl_->subscriptions=std::move(endpoints);impl_->frame_active->store(false,std::memory_order_release);impl_->history_active->store(false,std::memory_order_release);impl_->frame_active=std::make_shared<std::atomic_bool>(true);impl_->history_active=std::make_shared<std::atomic_bool>(true);}
    impl_->reproject_failed_sources();return true;
}
void NumericEngine::set_numeric_frame_handler(std::function<void(DeliveredNumericFrame)> handler){std::lock_guard lock(impl_->mutex);impl_->frame_active->store(false,std::memory_order_release);impl_->frame_active=std::make_shared<std::atomic_bool>(true);impl_->frame_handler=std::move(handler);}
void NumericEngine::set_numeric_history_handler(std::function<void(DeliveredNumericHistoryRow)> handler){std::lock_guard lock(impl_->mutex);impl_->history_active->store(false,std::memory_order_release);impl_->history_active=std::make_shared<std::atomic_bool>(true);impl_->history_handler=std::move(handler);}
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
    std::lock_guard lock(impl_->mutex);return impl_->process(node,frame);
}
Status NumericEngine::feed_numeric_owned(domain::NodeId node,const OwnedDataFrame& frame,
    std::optional<std::uint64_t> ordinal,std::shared_ptr<const void> retention_owner,std::optional<std::uint64_t> filled_count) {
    std::lock_guard lock(impl_->mutex);
    return impl_->executor.process_numeric_owned_ports(node,frame,{impl_->state.graph_revision,impl_->state.epoch},
        {[](void* p,const auto& f){static_cast<Impl*>(p)->output(f);},
         [](void* p,const auto& f){static_cast<Impl*>(p)->output(f);},
         [](void* p,const auto& f){static_cast<Impl*>(p)->history(f);},impl_.get()},ordinal,std::move(retention_owner),filled_count);
}
void NumericEngine::synchronize_control_state(const application::ProcessingReply& reply) {
    impl_->project(reply);
    std::lock_guard lock(impl_->mutex);impl_->capture_requested=reply.state.run_requested;
}
void NumericEngine::with_control_service(const std::function<void(application::ProcessingControlService&)>& call) {
    bool changed=false;
    {std::lock_guard control_lock(impl_->control_mutex);
        const auto prior=snapshot();
        call(*impl_->service);const auto reply=impl_->command(application::GetState{});
        impl_->project(reply);
        {std::lock_guard lock(impl_->mutex);impl_->capture_requested=reply.state.run_requested;
            changed=prior.graph_revision!=impl_->state.graph_revision;}}
    if(changed){impl_->reconcile_sources();if(impl_->snapshot_handler)impl_->snapshot_handler(snapshot());}
}
std::uint64_t NumericEngine::subscribe_numeric_tap(std::vector<EndpointIdentity> endpoints,
    std::function<void(DeliveredNumericFrame)> frame,std::function<void(DeliveredNumericHistoryRow)> history) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->taps.size()>=8 || endpoints.size()>2048)return 0;
    const auto token=++impl_->next_tap;
    impl_->taps.emplace(token,Impl::Tap{std::make_shared<std::atomic_bool>(true),std::move(endpoints),std::move(frame),std::move(history)});return token;
}
void NumericEngine::update_numeric_tap(std::uint64_t token,std::vector<EndpointIdentity> endpoints) {
    if(endpoints.size()>2048)return;
    if(endpoints.empty())endpoints.push_back({"",-1,domain::PortDirection::Output,0});
    std::lock_guard lock(impl_->mutex);auto it=impl_->taps.find(token);if(it==impl_->taps.end()||it->second.endpoints==endpoints)return;
    it->second.active->store(false,std::memory_order_release);it->second.active=std::make_shared<std::atomic_bool>(true);it->second.endpoints=std::move(endpoints);
}
void NumericEngine::unsubscribe_numeric_tap(std::uint64_t token) {std::lock_guard lock(impl_->mutex);if(auto it=impl_->taps.find(token);it!=impl_->taps.end()){it->second.active->store(false,std::memory_order_release);impl_->taps.erase(it);}}
Status NumericEngine::query_numeric(const EndpointIdentity& endpoint,DeliveredNumericFrame& out,std::uint64_t* ordinal,std::uint64_t* filled_count,bool* is_history) {
    if(is_history)*is_history=false;
    std::lock_guard lock(impl_->mutex);
    const auto context=impl_->contexts.find(endpoint);
    if(context==impl_->contexts.end() || !impl_->executor.plan() || !impl_->executor.context())return Status::unavailable;
    const auto history=impl_->history_ordinals.find(endpoint);
    if(history==impl_->history_ordinals.end())return Status::unsupported_version;
    DataFrameView borrowed;
    std::uint64_t occupancy=0,accepted_ordinal=0;
    const auto result=impl_->executor.plan()->history(endpoint.node,endpoint.port,*impl_->executor.context(),borrowed,&occupancy,&accepted_ordinal);
    if(result==Status::incompatible_contract){
        const auto cached=impl_->latest_matrices.find(endpoint);
        if(cached==impl_->latest_matrices.end() || !cached->second.frame)return Status::unavailable;
        out=cached->second;out.context=context->second;out.delivery={impl_->state.graph_revision,impl_->state.epoch};
        if(ordinal)*ordinal=0;if(filled_count)*filled_count=0;return out.status;
    }
    if(result!=Status::ok && result!=Status::unavailable)return result;
    if(is_history)*is_history=true;
    borrowed.stamp.graph_revision=impl_->state.graph_revision;borrowed.stamp.epoch=impl_->state.epoch;
    DeliveredNumericFrame candidate{context->second,{impl_->state.graph_revision,impl_->state.epoch},{},result};
    const auto retained=retain_frame(borrowed,frame_limits,impl_->arena->payload_pool,impl_->arena->metadata_pool,candidate.frame);
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
                application::ProcessingDTO dto;dto.numeric=graph;
                result=impl_->command(application::PrepareProcessing{&dto});
                impl_->project(result);
                if(result.accepted) {
                    // Platform selections are consumed only at runtime Apply.
                    // Restore a refused candidate before projecting its reply.
                    auto prior_devices=std::move(impl_->capture_devices);
                    impl_->capture_devices=std::move(capture_devices);
                    result=impl_->command(application::ApplyProcessing{result.prepared_token,graph.graph_id});
                    if(!result.accepted)impl_->capture_devices=std::move(prior_devices);
                }
                impl_->project(result);reply.accepted=result.accepted;reconcile=result.accepted;
                if(result.accepted)impl_->capture_requested=result.state.run_requested;
                if(!result.accepted)reply.error=failure(result.status)+QStringLiteral(" control=%1 validation=%2 node=%3").arg(int(result.error)).arg(int(result.numeric_validation.error)).arg(result.numeric_validation.node);
            }
        } else if(request.kind==EngineCommand::Snapshot || request.kind==EngineCommand::Subscribe)reply.accepted=true;
        else reply.error=QStringLiteral("Processing parameters belong to the numeric graph");
        reply.snapshot=impl_->state;
    }
    if(request.kind==EngineCommand::ApplyGraph && reply.accepted && adapter_handlers_.commit_graph)
        adapter_handlers_.commit_graph(request.graph);
    if(reconcile)impl_->reconcile_sources();
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

