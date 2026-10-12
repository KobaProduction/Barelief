#include "numeric_engine_ingress.h"
#include <QDebug>
#include <algorithm>

namespace pipeline::numeric_engine_detail {
static Status invalid(const char* phase,domain::NodeId node=-1,const char* field="") {
    qWarning("Numeric cold apply failed: phase=%s node=%d field=%s",phase,int(node),field);
    return Status::invalid_argument;
}
bool SourceIngress::physical_supported(const domain::ResolvedProcessingNode& node) noexcept {
        if(node.kind!="AudioSource" && node.kind!="TestSignal" && node.kind!="SignalGenerator")return true;
        if(node.outputs.empty() || node.outputs.size()>32 ||
            (node.kind!="AudioSource" && node.outputs.size()!=1))return false;
        std::uint32_t rate=0;
        for(const auto& port:node.outputs) {
            if(!port.contract)return false;
            const auto data=port.contract.view();const auto* c=std::get_if<NumericContract>(&data);
            if(!c || c->dtype!=DType::Float32 || c->representation.encoding!=StorageEncoding::Native ||
                c->shape.rank!=Rank::Vector || c->shape.axes[0]!=Axis::Sample || c->channel_order.size()!=1 ||
                !c->clock.sample_rate || c->clock.sample_rate>192000 || c->clock.tick_seconds.numerator!=1 ||
                c->clock.tick_seconds.denominator!=c->clock.sample_rate || !c->shape.extents[0].maximum ||
                c->shape.extents[0].maximum>8192 || c->coordinates[0].mode!=CoordinateMode::None)return false;
            if(rate && rate!=c->clock.sample_rate)return false;
            rate=c->clock.sample_rate;
        }
        if(node.kind!="SignalGenerator") {
            domain::ProcessingParameterView value;
            if(domain::resolve_processing_parameter(node,"sample_rate",value)!=Status::ok)return false;
            const auto* literal=std::get_if<NumericLiteral>(&value);
            if(!literal || std::visit([](auto x){return double(x);},*literal)!=rate)return false;
        }
        return true;
    }
Status SourceIngress::capture_reservation(const domain::NumericProcessingGraph& graph,std::size_t& bytes) noexcept {
        bytes=0;
        const auto add=[&](std::size_t value) {if(value>SIZE_MAX-bytes)return false;bytes+=value;return true;};
        const auto multiply=[](std::size_t a,std::size_t b,std::size_t& value) {
            if(b && a>SIZE_MAX/b)return false;value=a*b;return true;
        };
        for(const auto& node:graph.nodes)if(node.kind=="AudioSource" || node.kind=="TestSignal") {
            if(node.outputs.empty())return Status::incompatible_contract;
            const auto view=node.outputs.front().contract.view();const auto* numeric=std::get_if<NumericContract>(&view);
            if(!numeric)return Status::incompatible_contract;
            // Ring and reusable channel-major dispatch frame coexist. A source
            // owns one worker stack; reserve its bounded platform bookkeeping too.
            const auto frames=node.kind=="AudioSource"?std::max(1u,numeric->clock.sample_rate/4):std::max(1u,numeric->clock.sample_rate/200);
            std::size_t samples=0,payload=0;
            if(!multiply(node.outputs.size(),frames,samples) || !multiply(samples,sizeof(float)*2,payload) ||
                !add(payload) || !add(1024*1024+64*1024))return Status::overflow;
            for(const auto& port:node.outputs) {
                const auto data=port.contract.view();const auto* lane=std::get_if<NumericContract>(&data);
                std::size_t ingress=0;
                if(!lane)return Status::incompatible_contract;
                if(!multiply(std::size_t(lane->shape.extents[0].maximum),sizeof(float),ingress) ||
                    !add(ingress) || !add(sizeof(CaptureInput)) || !add(port.contract.owned_bytes()))return Status::overflow;
            }
        }
        return Status::ok;
    }

Status SourceIngress::stage(const domain::NumericProcessingGraph& graph,const domain::ProcessingAdmissionLimits& admission,Staged& staged) {
    const auto reservation=capture_reservation(graph,staged.capture_bytes);
    if(reservation!=Status::ok)return reservation;
        for(const auto& node:graph.nodes)if(node.kind=="NetworkInput" && !node.outputs.empty()) {
            const auto view=node.outputs[0].contract.view();const auto* numeric=std::get_if<NumericContract>(&view);
            if(!numeric || !numeric->clock.sample_rate || numeric->clock.tick_seconds.numerator<=0)continue;
            ExternalInput input;input.contract=node.outputs[0].contract;input.rate=numeric->clock.sample_rate;
            input.cadence=graph.execution_interval;input.origin_ns=steady_ns();
            if(numeric->shape.rank==Rank::Vector && numeric->shape.axes[0]==Axis::Sample) {
                input.block=std::size_t(numeric->shape.extents[0].maximum);
                input.cadence={std::int64_t(input.block),input.rate};
                input.stamp.sample_count=input.block;input.stamp.has_sample_interval=true;
            }
            input.stamp.producer_epoch=state.epoch;input.stamp.discontinuity=true;
            const auto prior=external_inputs.find(node.id);
            if(prior!=external_inputs.end() && same_contract(prior->second.contract.view(),view) && prior->second.cadence==input.cadence)input=prior->second;
            staged.external_inputs_candidate.emplace(node.id,std::move(input));
        }
        for(const auto& node:graph.nodes)if(const auto* clock=std::get_if<domain::SampleClockProcessingParameters>(&node.parameters)) {
            ClockInput input;input.node=node.id;input.rate=clock->sample_rate;
            input.block=std::max(1u,input.rate/120);input.origin_ns=steady_ns();input.producer_epoch=state.epoch+(++capture_fence);
            for(const auto& consumer:graph.nodes)if((consumer.kind=="SignalGenerator" || consumer.kind=="Mixer") && consumer.clock_node==node.id && !consumer.outputs.empty()) {
                const auto view=consumer.outputs[0].contract.view();const auto* numeric=std::get_if<NumericContract>(&view);
                if(!numeric)return Status::incompatible_contract;
                if(numeric->coordinates[0].mode==CoordinateMode::None)
                    input.block=std::min(input.block,std::size_t(numeric->shape.extents[0].maximum));
            }
            const auto prior=clocks.find(node.id);
            if(prior!=clocks.end() && prior->second.rate==input.rate) {
                const auto block=input.block;input=prior->second;input.block=block;
            }
            input.failed=false;staged.clocks_candidate.emplace(node.id,input);
        }
        for(const auto& node:graph.nodes) {
            domain::ResolvedProcessingNode resolved;
            if(!domain::resolve_processing_node(node,admission,resolved))return invalid("node-admission",node.id,node.kind.c_str());
            if(node.kind!="AudioSource" && node.kind!="TestSignal")continue;
            if(!physical_supported(resolved))return Status::incompatible_contract;
            for(std::size_t lane=0;lane<node.outputs.size();++lane) {
                const auto& port=node.outputs[lane];
                CaptureInput input;input.node=node.id;input.port=port.id;input.lane=lane;input.source_lanes=node.outputs.size();input.owned=port.contract;
                const auto contract=input.owned.view();const auto* numeric=std::get_if<NumericContract>(&contract);
                input.contract=*numeric;input.block=std::size_t(numeric->shape.extents[0].maximum);
                input.source=node.kind=="TestSignal"?AudioSourceKind::TestSignal:AudioSourceKind::SystemOutput;
                input.configuration["sample_rate"]=int(numeric->clock.sample_rate);
                input.configuration["channel_count"]=int(node.outputs.size());
                if(node.kind=="AudioSource")if(const auto device=capture_devices.find(node.id);device!=capture_devices.end())
                    input.configuration["device_id"]=device->second;
                if(node.kind=="TestSignal")for(const auto key:{"tones","minimum_hz","maximum_hz","amplitude","period_seconds","sweep_depth"}) {
                    domain::ProcessingParameterView value;
                    if(domain::resolve_processing_parameter(resolved,key,value)!=Status::ok)return invalid("parameter-resolve",node.id,key);
                    const auto* literal=std::get_if<NumericLiteral>(&value);if(!literal)return invalid("literal-parameter-type",node.id);
                    input.configuration[key]=std::visit([](auto v){return double(v);},*literal);
                }
                if(const auto* params=std::get_if<domain::EndpointProcessingParameters>(&node.parameters))
                    for(const auto& p:params->values)if(p.key=="source") {
                        const auto* literal=std::get_if<NumericLiteral>(&p.value);if(!literal)return invalid("literal-parameter-type",node.id);
                        const auto selected=std::visit([](auto x){return double(x);},*literal);
                        if(selected!=0 && selected!=1)return invalid("capture-source",node.id);
                        input.source=AudioSourceKind(int(selected));
                    }
                input.bytes.resize(input.block*sizeof(float));input.planes[0]={0,{sizeof(float),0}};
                const auto prior=std::find_if(inputs.begin(),inputs.end(),[&](const auto& p) {
                    return p.node==input.node && p.port==input.port && p.source==input.source &&
                        p.lane==input.lane && p.source_lanes==input.source_lanes &&
                        (p.configuration==input.configuration || input.source==AudioSourceKind::TestSignal) && same_contract(p.owned.view(),input.owned.view());
                });
                if(prior!=inputs.end()) {
                    auto configuration=input.configuration;input=*prior;input.configuration=std::move(configuration);
                } else if(const auto token=source_tokens.find(input.node);token!=source_tokens.end())input.producer_epoch=state.epoch+token->second;
                // OwnedProcessingContract copies its backing arrays; never retain
                // spans into the retiring previous CaptureInput after Apply.
                const auto rebound=input.owned.view();input.contract=std::get<NumericContract>(rebound);
                staged.inputs_candidate.push_back(std::move(input));
            }
        }
    return Status::ok;
}
void SourceIngress::commit(Staged&& staged) {
    working_capture_bytes=staged.capture_bytes;
    inputs.swap(staged.inputs_candidate);clocks.swap(staged.clocks_candidate);external_inputs.swap(staged.external_inputs_candidate);
}
Status DeliveryHub::stage(const domain::NumericProcessingGraph& graph,const domain::ProcessingAdmissionLimits& admission,Staged& staged) {
        for(const auto& node:graph.nodes) {
            domain::ResolvedProcessingNode resolved;
            if(!domain::resolve_processing_node(node,admission,resolved))return invalid("node-admission",node.id,node.kind.c_str());
            const auto add=[&](const domain::ProcessingPort& port) {
                auto context=std::make_shared<NumericDeliveryContext>();
                context->endpoint={graph.graph_id,node.id,port.direction,port.id};
                context->contract=port.contract;context->retention_owner=retention_arenas;
                const auto data=port.contract.view();
                if(const auto* numeric=std::get_if<NumericContract>(&data);numeric && numeric->shape.rank==Rank::Matrix) {
                    const auto prior=history_ordinals.find(context->endpoint);
                    const auto old=contexts.find(context->endpoint);
                    const bool compatible=old!=contexts.end() && same_contract(old->second->contract.view(),data);
                    staged.history_ordinals_candidate.emplace(context->endpoint,!compatible || prior==history_ordinals.end()?0:prior->second);
                    auto cached=latest_matrices.find(context->endpoint);DeliveredNumericFrame retained;
                    if(compatible && cached!=latest_matrices.end()){retained=cached->second;retained.context=context;}
                    staged.latest_matrices_candidate.emplace(context->endpoint,std::move(retained));
                }
                const auto pending=delivery_gaps.find(context->endpoint);
                const auto old_context=contexts.find(context->endpoint);
                staged.delivery_gaps_candidate.emplace(context->endpoint,pending!=delivery_gaps.end()&&pending->second&&old_context!=contexts.end()&&same_contract(old_context->second->contract.view(),data));
                staged.contexts_candidate.emplace(context->endpoint,std::move(context));
            };
            for(const auto& port:node.outputs)add(port);
            if(resolved.role==domain::NodeRole::Sink)for(const auto& port:node.inputs)add(port);
            if(node.kind=="WLEDOutput" && node.enabled) {
                WledOutput output;std::uint32_t cadence=17;
                for(const auto key:{"destination","interface","port","cadence_ms"}) {
                    domain::ProcessingParameterView value;
                    if(domain::resolve_processing_parameter(resolved,key,value)!=Status::ok)return invalid("parameter-resolve",node.id,key);
                    if(const auto* text=std::get_if<std::string_view>(&value)) {
                        QHostAddress address(QString::fromUtf8(text->data(),int(text->size())));
                        if(address.isNull())return invalid("WLED-address",node.id,key);
                        if(std::string_view(key)=="destination")output.destination=address;else output.interface=address;
                    } else if(const auto* number=std::get_if<NumericLiteral>(&value)) {
                        const auto n=std::visit([](auto x){return std::uint32_t(x);},*number);
                        if(std::string_view(key)=="port")output.port=quint16(n);else cadence=n;
                    } else return invalid("WLED-parameter-type",node.id,key);
                }
                output.policy.set_cadence(cadence);
                const auto old=wled_outputs.find(node.id);
                if(old!=wled_outputs.end() && old->second.destination==output.destination && old->second.interface==output.interface && old->second.port==output.port) {
                    output=old->second;output.policy.set_cadence(cadence);
                } else output.socket=std::make_shared<QUdpSocket>();
                if(!output.policy.running())output.policy.start(steady_ns()/1000000);
                staged.wled_outputs_candidate.emplace(node.id,std::move(output));
            }
        }
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
