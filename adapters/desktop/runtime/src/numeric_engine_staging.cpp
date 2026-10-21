#include "numeric_engine_ingress.h"
#include <QDebug>
#include <algorithm>

namespace pipeline::numeric_engine_detail {
static Status invalid(const char* phase,domain::NodeId node=-1,const char* field="") {
    qWarning("Numeric cold apply failed: phase=%s node=%d field=%s",phase,int(node),field);
    return Status::invalid_argument;
}
bool SourceIngress::physical_rate_supported(const domain::ResolvedProcessingNode& node,std::uint32_t rate) noexcept {
        if(node.kind!="SignalGenerator") {
            domain::ProcessingParameterView value;
            if(domain::resolve_processing_parameter(node,"sample_rate",value)!=Status::ok)return false;
            const auto* literal=std::get_if<NumericLiteral>(&value);
            if(!literal || std::visit([](auto x){return double(x);},*literal)!=rate)return false;
        }
        return true;
}
bool SourceIngress::physical_supported(const domain::ResolvedProcessingNode& node) noexcept {
        if(node.kind!="AudioSource" && node.kind!="TestSignal" && node.kind!="SignalGenerator")return true;
        if(node.outputs.empty() || node.outputs.size()>32 ||
            (node.kind!="AudioSource" && node.outputs.size()!=1))return false;
        std::uint32_t rate=0;
        for(const auto& port:node.outputs)if(!physical_output_supported(port,rate))return false;
        return physical_rate_supported(node,rate);
    }
bool SourceIngress::physical_output_supported(const domain::ProcessingPort& port,std::uint32_t& rate) noexcept {
    if(!port.contract)return false;
    const auto data=port.contract.view();const auto* c=std::get_if<NumericContract>(&data);
    if(!c || c->dtype!=DType::Float32 || c->representation.encoding!=StorageEncoding::Native ||
       c->shape.rank!=Rank::Vector || c->shape.axes[0]!=Axis::Sample || c->channel_order.size()!=1 ||
       !c->clock.sample_rate || c->clock.sample_rate>192000 || c->clock.tick_seconds.numerator!=1 ||
       c->clock.tick_seconds.denominator!=c->clock.sample_rate || !c->shape.extents[0].maximum ||
       c->shape.extents[0].maximum>8192 || c->coordinates[0].mode!=CoordinateMode::None)return false;
    if(rate && rate!=c->clock.sample_rate)return false;
    rate=c->clock.sample_rate;
    return true;
}
static bool reserve_add(std::size_t value,std::size_t& bytes) noexcept {
    if(value>SIZE_MAX-bytes)return false;bytes+=value;return true;
}
static bool reserve_multiply(std::size_t a,std::size_t b,std::size_t& value) noexcept {
    if(b && a>SIZE_MAX/b)return false;value=a*b;return true;
}
Status SourceIngress::capture_reservation(const domain::NumericProcessingGraph& graph,std::size_t& bytes) noexcept {
        bytes=0;
        for(const auto& node:graph.nodes)if(node.kind=="AudioSource" || node.kind=="TestSignal") {
            const auto status=reserve_capture_node(node,bytes);
            if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
std::size_t SourceIngress::reserved_capture_lanes(const domain::ConfiguredProcessingNode& node) noexcept {
    const auto lanes=domain::data_outputs(node).size();
    if(node.kind!="AudioSource")return lanes;
    const auto* fields=std::get_if<domain::EndpointProcessingParameters>(&node.parameters);
    if(!fields)return lanes;
    for(const auto& p:fields->values) {
        if(p.key!="output_count")continue;
        const auto* value=std::get_if<NumericLiteral>(&p.value);
        if(value && std::visit([](auto n){return n>0;},*value))return 64;
    }
    return lanes;
}
Status SourceIngress::reserve_capture_ports(const domain::ConfiguredProcessingNode& node,std::size_t& bytes) noexcept {
            for(const auto& port:domain::data_outputs(node)) {
                const auto status=reserve_capture_port(port,bytes);
                if(status!=Status::ok)return status;
            }
            return Status::ok;
}
Status SourceIngress::reserve_capture_node(const domain::ConfiguredProcessingNode& node,std::size_t& bytes) noexcept {
            if(node.outputs.empty())return Status::incompatible_contract;
            const auto view=node.outputs.front().contract.view();const auto* numeric=std::get_if<NumericContract>(&view);
            if(!numeric)return Status::incompatible_contract;
            // Ring and reusable channel-major dispatch frame coexist. A source
            // owns one worker stack; reserve its bounded platform bookkeeping too.
            const auto frames=node.kind=="AudioSource"?std::max(1u,numeric->clock.sample_rate/4):std::max(1u,numeric->clock.sample_rate/200);
            std::size_t samples=0,payload=0;
            const auto capture_lanes=reserved_capture_lanes(node);
            if(!reserve_multiply(capture_lanes,frames,samples) || !reserve_multiply(samples,sizeof(float)*2,payload) ||
                !reserve_add(payload,bytes) || !reserve_add(1024*1024+64*1024,bytes))return Status::overflow;
            return reserve_capture_ports(node,bytes);
    }
Status SourceIngress::reserve_capture_port(const domain::ProcessingPort& port,std::size_t& bytes) noexcept {
                const auto data=port.contract.view();const auto* lane=std::get_if<NumericContract>(&data);
                std::size_t ingress=0;
                if(!lane)return Status::incompatible_contract;
                if(!reserve_multiply(std::size_t(lane->shape.extents[0].maximum),sizeof(float),ingress) ||
                    !reserve_add(ingress,bytes) || !reserve_add(ingress,bytes) ||
                    !reserve_add(dataflow_runtime::PcmPacketQueue::storage_bytes(std::size_t(lane->shape.extents[0].maximum)*2,4),bytes) ||
                    !reserve_add(sizeof(CaptureInput),bytes) || !reserve_add(port.contract.owned_bytes(),bytes))return Status::overflow;
        return Status::ok;
    }

namespace {
void strip_sample_rate_outputs(domain::NumericProcessingGraph& graph) {
    for(auto& node:graph.nodes)if(domain::has_sample_rate_output(node.kind))node.outputs.resize(domain::data_outputs(node).size());
}
}
Status SourceIngress::stage_timelines(const domain::NumericProcessingGraph& graph,Staged& staged) {
    const auto clock_status=stage_sample_clocks(graph,staged);
    if(clock_status!=Status::ok)return clock_status;
    const auto generator_status=stage_unbound_generators(graph,staged);
    if(generator_status!=Status::ok)return generator_status;
    stage_constants(graph,staged);
    return Status::ok;
}
Status SourceIngress::stage(const domain::NumericProcessingGraph& requested,const domain::ProcessingAdmissionLimits& admission,Staged& staged) {
    auto graph=requested;
    strip_sample_rate_outputs(graph);
    const auto reservation=capture_reservation(graph,staged.capture_bytes);
    if(reservation!=Status::ok)return reservation;
    stage_external_inputs(graph,staged);
    const auto timelines=stage_timelines(graph,staged);
    if(timelines!=Status::ok)return timelines;
    return stage_capture_inputs(graph,admission,staged);
}
void SourceIngress::configure_external_cadence(ExternalInput& input,const NumericContract& contract) {
    const auto* numeric=&contract;
            if(numeric->shape.rank==Rank::Vector && numeric->shape.axes[0]==Axis::Sample) {
                input.block=std::size_t(numeric->shape.extents[0].maximum);
                input.cadence={std::int64_t(input.block),input.rate};
                input.stamp.sample_count=input.block;input.stamp.has_sample_interval=true;
            }
}
void SourceIngress::stage_external_node(const domain::NumericProcessingGraph& graph,const domain::ConfiguredProcessingNode& node,Staged& staged) {
            const auto view=node.outputs[0].contract.view();const auto* numeric=std::get_if<NumericContract>(&view);
            if(!numeric || !numeric->clock.sample_rate || numeric->clock.tick_seconds.numerator<=0)return;
            ExternalInput input;input.contract=node.outputs[0].contract;input.rate=numeric->clock.sample_rate;
            input.cadence=graph.execution_interval;input.origin_ns=steady_ns();
            configure_external_cadence(input,*numeric);
            input.stamp.producer_epoch=state.epoch;input.stamp.discontinuity=true;
            const auto prior=external_inputs.find(node.id);
            if(prior!=external_inputs.end() && same_contract(prior->second.contract.view(),view) && prior->second.cadence==input.cadence)input=prior->second;
            staged.external_inputs_candidate.emplace(node.id,std::move(input));
}
void SourceIngress::stage_external_inputs(const domain::NumericProcessingGraph& graph,Staged& staged) {
        for(const auto& node:graph.nodes)if(node.kind=="NetworkInput" && !node.outputs.empty()) {
            stage_external_node(graph,node,staged);
        }
}
Status SourceIngress::limit_clock_block(const domain::NumericProcessingGraph& graph,const domain::ConfiguredProcessingNode& node,ClockInput& input) {
            for(const auto& consumer:graph.nodes)if((consumer.kind=="SignalGenerator" || consumer.kind=="Mixer") && consumer.clock_node==node.id && !consumer.outputs.empty()) {
                const auto view=consumer.outputs[0].contract.view();const auto* numeric=std::get_if<NumericContract>(&view);
                if(!numeric)return Status::incompatible_contract;
                if(numeric->coordinates[0].mode==CoordinateMode::None)
                    input.block=std::min(input.block,std::size_t(numeric->shape.extents[0].maximum));
            }
    return Status::ok;
}
void SourceIngress::retain_staged_clock(domain::NodeId id,ClockInput input,Staged& staged) {
            const auto prior=clocks.find(id);
            if(prior!=clocks.end() && prior->second.rate==input.rate) {
                const auto block=input.block;input=prior->second;input.block=block;
            }
            input.failed=false;staged.clocks_candidate.emplace(id,input);
}
Status SourceIngress::stage_sample_clocks(const domain::NumericProcessingGraph& graph,Staged& staged) {
        for(const auto& node:graph.nodes)if(const auto* clock=std::get_if<domain::SampleClockProcessingParameters>(&node.parameters)) {
            ClockInput input;input.node=node.id;input.rate=clock->sample_rate;
            input.block=std::max(1u,input.rate/120);input.origin_ns=steady_ns();input.producer_epoch=state.epoch+(++capture_fence);
            const auto status=limit_clock_block(graph,node,input);
            if(status!=Status::ok)return status;
            retain_staged_clock(node.id,input,staged);
        }
        return Status::ok;
}
Status SourceIngress::stage_unbound_generator(const domain::ConfiguredProcessingNode& node,Staged& staged) {
            if(node.outputs.empty())return Status::incompatible_contract;
            const auto data=node.outputs.front().contract.view();const auto* numeric=std::get_if<NumericContract>(&data);
            if(!numeric || !numeric->clock.sample_rate)return Status::incompatible_contract;
            ClockInput input;input.node=node.id;input.rate=numeric->clock.sample_rate;
            input.block=std::min(std::size_t(std::max(1u,input.rate/120)),std::size_t(numeric->shape.extents[0].maximum));
            input.block=std::max(input.block,std::size_t(numeric->shape.extents[0].minimum));
            input.origin_ns=steady_ns();input.producer_epoch=state.epoch+(++capture_fence);
            retain_staged_clock(node.id,input,staged);
    return Status::ok;
}
Status SourceIngress::stage_unbound_generators(const domain::NumericProcessingGraph& graph,Staged& staged) {
        // An unbound generator advances its own prepared timeline. An explicit
        // synchronization binding replaces this schedule with the shared one.
        for(const auto& node:graph.nodes)if(node.kind=="SignalGenerator" && node.clock_node<0) {
            const auto status=stage_unbound_generator(node,staged);
            if(status!=Status::ok)return status;
        }
        return Status::ok;
}
void SourceIngress::stage_constants(const domain::NumericProcessingGraph& graph,Staged& staged) {
        for(const auto& node:graph.nodes)if(domain::numeric_constant_kind(node.kind)){
            ClockInput input;input.node=node.id;input.rate=120;input.block=1;input.origin_ns=steady_ns();input.producer_epoch=state.epoch+(++capture_fence);
            const auto prior=clocks.find(node.id);if(prior!=clocks.end())input=prior->second;
            input.failed=false;staged.clocks_candidate.emplace(node.id,input);
        }
}
Status SourceIngress::stage_capture_inputs(const domain::NumericProcessingGraph& graph,
                                           const domain::ProcessingAdmissionLimits& admission,Staged& staged) {
        for(const auto& node:graph.nodes) {
            domain::ResolvedProcessingNode resolved;
            if(!domain::resolve_processing_node(node,admission,resolved))return invalid("node-admission",node.id,node.kind.c_str());
            if(node.kind!="AudioSource" && node.kind!="TestSignal")continue;
            if(!physical_supported(resolved))return Status::incompatible_contract;
            const auto status=stage_capture_node(node,resolved,staged);
            if(status!=Status::ok)return status;
        }
    return Status::ok;
}
Status SourceIngress::stage_capture_node(const domain::ConfiguredProcessingNode& node,
                                         const domain::ResolvedProcessingNode& resolved,Staged& staged) {
            for(std::size_t lane=0;lane<node.outputs.size();++lane) {
                CaptureInput input;
                const auto status=configure_capture_input(node,resolved,lane,input);
                if(status!=Status::ok)return status;
                retain_capture_state(input);
                staged.inputs_candidate.push_back(std::move(input));
            }
    return Status::ok;
}
Status SourceIngress::prepare_capture_storage(CaptureInput& input,std::uint32_t rate) {
    input.bytes.resize(input.block*sizeof(float));input.planes[0]={0,{sizeof(float),0}};
    input.packet_scratch.resize(input.block);
    if(!input.packets.prepare(rate,input.block*2,4))return Status::incompatible_contract;
    return Status::ok;
}
Status SourceIngress::configure_capture_input(const domain::ConfiguredProcessingNode& node,
    const domain::ResolvedProcessingNode& resolved,std::size_t lane,CaptureInput& input) {
    const auto& port=node.outputs[lane];
    input.node=node.id;input.port=port.id;input.lane=lane;input.source_lanes=node.outputs.size();input.owned=port.contract;
    const auto contract=input.owned.view();const auto* numeric=std::get_if<NumericContract>(&contract);
    input.contract=*numeric;input.block=std::size_t(numeric->shape.extents[0].maximum);
    input.source=node.kind=="TestSignal"?AudioSourceKind::TestSignal:AudioSourceKind::SystemOutput;
    input.configuration["sample_rate"]=int(numeric->clock.sample_rate);
    input.configuration["channel_count"]=int(node.outputs.size());
    const auto status=configure_capture_parameters(node,resolved,input);
    if(status!=Status::ok)return status;
    return prepare_capture_storage(input,numeric->clock.sample_rate);
}
Status SourceIngress::configure_capture_parameters(const domain::ConfiguredProcessingNode& node,
    const domain::ResolvedProcessingNode& resolved,CaptureInput& input) {
    if(node.kind=="AudioSource"){
        const auto status=configure_audio_capture(node,resolved,input);
        if(status!=Status::ok)return status;
    }
    if(node.kind=="TestSignal"){
        const auto status=configure_test_signal(node,resolved,input);
        if(status!=Status::ok)return status;
    }
    return configure_capture_source(node,input);
}
Status SourceIngress::configure_audio_capture(const domain::ConfiguredProcessingNode& node,
    const domain::ResolvedProcessingNode& resolved,CaptureInput& input) {
        domain::ProcessingParameterView count;
        if(domain::resolve_processing_parameter(resolved,"output_count",count)!=Status::ok)return invalid("output-count",node.id);
        const auto* literal=std::get_if<NumericLiteral>(&count);if(!literal)return invalid("output-count-type",node.id);
        input.select_native_lanes=std::visit([](auto value){return value>0;},*literal);
        if(input.select_native_lanes)input.configuration["channel_count"]=0;
        if(const auto device=capture_devices.find(node.id);device!=capture_devices.end())
            input.configuration["device_id"]=device->second;
        return Status::ok;
}
Status SourceIngress::configure_test_signal(const domain::ConfiguredProcessingNode& node,
    const domain::ResolvedProcessingNode& resolved,CaptureInput& input) {
    for(const auto key:{"tones","minimum_hz","maximum_hz","amplitude","period_seconds","sweep_depth"}) {
        domain::ProcessingParameterView value;
        if(domain::resolve_processing_parameter(resolved,key,value)!=Status::ok)return invalid("parameter-resolve",node.id,key);
        const auto* literal=std::get_if<NumericLiteral>(&value);if(!literal)return invalid("literal-parameter-type",node.id);
        input.configuration[key]=std::visit([](auto v){return double(v);},*literal);
    }
    return Status::ok;
}
Status SourceIngress::configure_capture_source(const domain::ConfiguredProcessingNode& node,CaptureInput& input) {
    if(const auto* params=std::get_if<domain::EndpointProcessingParameters>(&node.parameters))
        for(const auto& parameter:params->values)if(parameter.key=="source") {
            const auto* literal=std::get_if<NumericLiteral>(&parameter.value);
            if(!literal)return invalid("literal-parameter-type",node.id);
            const auto selected=std::visit([](auto value){return double(value);},*literal);
            if(selected!=0 && selected!=1)return invalid("capture-source",node.id);
            input.source=AudioSourceKind(int(selected));
        }
    return Status::ok;
}
void SourceIngress::retain_capture_state(CaptureInput& input) {
    const auto prior=std::find_if(inputs.begin(),inputs.end(),[&](const auto& previous) {
        return previous.node==input.node && previous.port==input.port && previous.source==input.source &&
            previous.lane==input.lane && previous.source_lanes==input.source_lanes &&
            previous.select_native_lanes==input.select_native_lanes &&
            (previous.configuration==input.configuration || input.source==AudioSourceKind::TestSignal) &&
            same_contract(previous.owned.view(),input.owned.view());
    });
    if(prior!=inputs.end()) {
        auto configuration=input.configuration;input=*prior;input.configuration=std::move(configuration);
    } else if(const auto token=source_tokens.find(input.node);token!=source_tokens.end())
        input.producer_epoch=state.epoch+token->second;
    const auto rebound=input.owned.view();input.contract=std::get<NumericContract>(rebound);
}
void SourceIngress::commit(Staged&& staged) {
    working_capture_bytes=staged.capture_bytes;
    inputs.swap(staged.inputs_candidate);clocks.swap(staged.clocks_candidate);external_inputs.swap(staged.external_inputs_candidate);
}
void DeliveryHub::stage_node_ports(const domain::NumericProcessingGraph& graph,const domain::ConfiguredProcessingNode& node,domain::NodeRole role,Staged& staged) {
            for(const auto& port:node.outputs)stage_port(graph,node,port,staged);
            for(const auto& port:node.inputs)
                if(role==domain::NodeRole::Sink || node.kind=="Reroute" ||
                   !domain::numeric_parameter_key(node.kind,port.id).empty())stage_port(graph,node,port,staged);
}
Status DeliveryHub::stage(const domain::NumericProcessingGraph& graph,const domain::ProcessingAdmissionLimits& admission,Staged& staged) {
        for(const auto& node:graph.nodes) {
            domain::ResolvedProcessingNode resolved;
            if(!domain::resolve_processing_node(node,admission,resolved))return invalid("node-admission",node.id,node.kind.c_str());
            stage_node_ports(graph,node,resolved.role,staged);
            if(node.kind=="WLEDOutput" && node.enabled) {
                const auto status=stage_wled(node,resolved,staged);
                if(status!=Status::ok)return status;
            }
        }
    return Status::ok;
}
void DeliveryHub::stage_history_port(const std::shared_ptr<NumericDeliveryContext>& context,const DataContract& data,Staged& staged) {
    if(const auto* numeric=std::get_if<NumericContract>(&data);numeric && numeric->shape.rank==Rank::Matrix) {
        const auto prior=history_ordinals.find(context->endpoint);
        const auto old=contexts.find(context->endpoint);
        const bool compatible=old!=contexts.end() && same_contract(old->second->contract.view(),data);
        staged.history_ordinals_candidate.emplace(context->endpoint,!compatible || prior==history_ordinals.end()?0:prior->second);
        auto cached=latest_matrices.find(context->endpoint);DeliveredNumericFrame retained;
        if(compatible && cached!=latest_matrices.end()){retained=cached->second;retained.context=context;}
        staged.latest_matrices_candidate.emplace(context->endpoint,std::move(retained));
    }
}
void DeliveryHub::stage_port(const domain::NumericProcessingGraph& graph,const domain::ConfiguredProcessingNode& node,
                             const domain::ProcessingPort& port,Staged& staged) {
    auto context=std::make_shared<NumericDeliveryContext>();
    context->endpoint={graph.graph_id,node.id,port.direction,port.id};
    context->contract=port.contract;context->retention_owner=retention_arenas;
    const auto data=port.contract.view();
    stage_history_port(context,data,staged);
    const auto pending=delivery_gaps.find(context->endpoint);
    const auto old_context=contexts.find(context->endpoint);
    staged.delivery_gaps_candidate.emplace(context->endpoint,pending!=delivery_gaps.end()&&pending->second&&
        old_context!=contexts.end()&&same_contract(old_context->second->contract.view(),data));
    staged.contexts_candidate.emplace(context->endpoint,std::move(context));
}
Status DeliveryHub::assign_wled_address(domain::NodeId id,const char* key,std::string_view text,WledOutput& output) {
            QHostAddress address(QString::fromUtf8(text.data(),int(text.size())));
            if(address.isNull())return invalid("WLED-address",id,key);
            if(std::string_view(key)=="destination")output.destination=address;else output.interface=address;
    return Status::ok;
}
Status DeliveryHub::assign_wled_parameter(domain::NodeId id,const char* key,const domain::ProcessingParameterView& value,WledOutput& output,std::uint32_t& cadence) {
        if(const auto* text=std::get_if<std::string_view>(&value)) {
            return assign_wled_address(id,key,*text,output);
        } else if(const auto* number=std::get_if<NumericLiteral>(&value)) {
            const auto numeric_value=std::visit([](auto item){return std::uint32_t(item);},*number);
            if(std::string_view(key)=="port")output.port=quint16(numeric_value);else cadence=numeric_value;
        } else return invalid("WLED-parameter-type",id,key);
    return Status::ok;
}
Status DeliveryHub::configure_wled_output(const domain::ConfiguredProcessingNode& node,
    const domain::ResolvedProcessingNode& resolved,WledOutput& output,std::uint32_t& cadence) {
    for(const auto key:{"destination","interface","port","cadence_ms"}) {
        domain::ProcessingParameterView value;
        if(domain::resolve_processing_parameter(resolved,key,value)!=Status::ok)return invalid("parameter-resolve",node.id,key);
        const auto status=assign_wled_parameter(node.id,key,value,output,cadence);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status DeliveryHub::stage_wled(const domain::ConfiguredProcessingNode& node,
                               const domain::ResolvedProcessingNode& resolved,Staged& staged) {
    WledOutput output;std::uint32_t cadence=17;
    const auto status=configure_wled_output(node,resolved,output,cadence);
    if(status!=Status::ok)return status;
    output.policy.set_cadence(cadence);
    const auto old=wled_outputs.find(node.id);
    if(old!=wled_outputs.end() && old->second.destination==output.destination &&
       old->second.interface==output.interface && old->second.port==output.port) {
        output=old->second;output.policy.set_cadence(cadence);
    } else output.socket=std::make_shared<QUdpSocket>();
    if(!output.policy.running())output.policy.start(steady_ns()/1000000);
    staged.wled_outputs_candidate.emplace(node.id,std::move(output));
    return Status::ok;
}
void DeliveryHub::commit(Staged&& staged) {
    contexts.swap(staged.contexts_candidate);history_ordinals.swap(staged.history_ordinals_candidate);
    latest_matrices.swap(staged.latest_matrices_candidate);delivery_gaps.swap(staged.delivery_gaps_candidate);wled_outputs.swap(staged.wled_outputs_candidate);
    if(!wled_outputs.empty()) {
        auto cadence=std::uint32_t(60000);for(const auto& [node,output]:wled_outputs)cadence=std::min(cadence,output.policy.cadence_ms());
        wled_timer.setInterval(int(cadence));wled_timer.start();
    } else wled_timer.stop();
    ++delivery_generation;frames.clear();previews.clear();rows.clear();preview_rows.clear();
}
void DeliveryHub::clear() {
    ++delivery_generation;preview_scheduled=false;preview_last_flush_ns=0;
    frames.clear();previews.clear();rows.clear();preview_rows.clear();wled_outputs.clear();
    contexts.clear();history_ordinals.clear();latest_matrices.clear();delivery_gaps.clear();
}
}
