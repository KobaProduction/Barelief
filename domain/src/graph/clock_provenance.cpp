#include "processing_admission.h"
#include <domain/graph/preparation_values.h>
namespace domain::detail {
namespace {
using namespace signal_contracts;
enum class OriginKind { Unavailable,Silence,Preparation,Producer };
struct ClockOrigin {
    OriginKind kind=OriginKind::Unavailable;
    NodeId producer=-1;
    static ClockOrigin produced(NodeId id) noexcept {return {OriginKind::Producer,id};}
};
bool combine(ClockOrigin& current,ClockOrigin next) noexcept {
    if(next.kind==OriginKind::Silence || next.kind==OriginKind::Preparation)return true;
    if(next.kind==OriginKind::Unavailable)return false;
    if(current.kind==OriginKind::Producer && current.producer!=next.producer)return false;
    current=next;return true;
}
struct ClockProvenance {
    const NumericProcessingGraph& graph;
    const Edge* parent(NodeId target,PortId input) const noexcept {
        for(const auto& edge:graph.edges)if(edge.to==target && edge.input_port==input)return &edge;
        return nullptr;
    }
    ClockOrigin source(const ConfiguredProcessingNode& node) const noexcept {
        if(node.kind=="AudioSource" || node.kind=="NetworkInput" || node.kind=="TestSignal")return ClockOrigin::produced(node.id);
        if(node.kind!="SignalGenerator")return {};
        for(const auto& binding:graph.configuration_bindings)if(binding.to==node.id && binding.input==ConfigurationRole::SampleClock)return ClockOrigin::produced(binding.from);
        return ClockOrigin::produced(node.id);
    }
    ClockOrigin edge_origin(const Edge& edge,std::size_t depth) const noexcept {
        return origin(edge.from,edge.output_port,depth+1);
    }
    bool selector_group(const ConfiguredProcessingNode& node,std::size_t group,std::size_t depth) const noexcept {
        ClockOrigin common;
        for(std::size_t lane=0;lane<node.outputs.size();++lane) {
            const auto* edge=parent(node.id,PortId(group*node.outputs.size()+lane));
            if(edge && !combine(common,edge_origin(*edge,depth)))return false;
        }
        return true;
    }
    ClockOrigin selector(const ConfiguredProcessingNode& node,std::size_t depth) const noexcept {
        if(node.outputs.empty() || node.inputs.size()%node.outputs.size())return {};
        for(std::size_t group=0;group<node.inputs.size()/node.outputs.size();++group)if(!selector_group(node,group,depth))return {};
        return ClockOrigin::produced(node.id);
    }
    std::optional<PortId> passthrough_input(const ConfiguredProcessingNode& node,PortId output) const noexcept {
        if(node.kind=="Switch") {
            const auto* route=std::get_if<RouteProcessingParameters>(&node.parameters);
            if(!route || !route->channels_per_stream)return {};
            return PortId(output%route->channels_per_stream);
        }
        std::optional<PortId> input;
        for(const auto& port:node.inputs)if(numeric_parameter_key(node.kind,port.id).empty()) {
            if(input)return {};
            input=port.id;
        }
        return input;
    }
    ClockOrigin disconnected(const ProcessingPort& endpoint) const noexcept {
        const auto contract=endpoint.contract.view();const auto* numeric=std::get_if<NumericContract>(&contract);
        if(numeric && numeric->clock.sample_rate && !numeric->clock.domain.empty())return {OriginKind::Silence};
        return {};
    }
    bool preparation(const ConfiguredProcessingNode& node,const ProcessingPort& endpoint) const noexcept {
        const auto contract=endpoint.contract.view();const auto* numeric=std::get_if<NumericContract>(&contract);
        if(!numeric || numeric->shape.rank!=Rank::Scalar)return false;
        const auto capability=preparation_value_capability(node,endpoint.id);
        if(capability.kind==PreparationValueKind::UnsignedInteger || capability.kind==PreparationValueKind::FiniteScalar)return true;
        if(capability.kind!=PreparationValueKind::Routed)return false;
        NumericLiteral literal;
        return resolve_preparation_value(graph,node.id,endpoint.id,literal)==Status::ok;
    }
    ClockOrigin routed_origin(const ConfiguredProcessingNode& node,const ProcessingPort& endpoint,PortId output,std::size_t depth) const noexcept {
        const auto input=passthrough_input(node,output);if(!input)return {};
        const auto* edge=parent(node.id,*input);
        return edge?edge_origin(*edge,depth):disconnected(endpoint);
    }
    ClockOrigin endpoint_origin(const ConfiguredProcessingNode& node,const ProcessingPort& endpoint,PortId output,std::size_t depth) const noexcept {
        if(preparation(node,endpoint))return {OriginKind::Preparation};
        const auto own=source(node);if(own.kind==OriginKind::Producer)return own;
        if(node.kind=="InputSelector")return selector(node,depth);
        return routed_origin(node,endpoint,output,depth);
    }
    ClockOrigin origin(NodeId id,PortId output,std::size_t depth) const noexcept {
        if(depth>=graph.nodes.size())return {};
        const auto index=admission_node(graph,id);if(index==graph.nodes.size())return {};
        const auto& node=graph.nodes[index];const auto* endpoint=admission_port(node.outputs,output);
        if(!endpoint)return {};
        return endpoint_origin(node,*endpoint,output,depth);
    }
    ProcessingDiagnostic aligned_inputs(const ConfiguredProcessingNode& node) const noexcept {
        ClockOrigin common;
        for(const auto& input:node.inputs) {
            const auto* edge=parent(node.id,input.id);if(!edge)continue;
            if(!combine(common,origin(edge->from,edge->output_port,0)))return {ProcessingError::IncompatiblePorts,node.id,input.id,std::size_t(edge-graph.edges.data())};
        }
        return {};
    }
};
bool requires_alignment(const ConfiguredProcessingNode& node) noexcept {
    if(node.kind=="WLEDEncode" || node.kind=="Fourier" || node.kind=="ChannelComparison")return true;
    const auto* mixer=std::get_if<MixerProcessingParameters>(&node.parameters);
    return node.kind=="Mixer" && mixer && mixer->consumption==MixerConsumption::SourceAligned;
}
ProcessingDiagnostic window_cadence(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& node) noexcept {
    if(node.kind!="Window")return {};
    const auto* endpoint=admission_port(node.inputs,0);if(!endpoint)return {ProcessingError::InvalidPort,node.id};
    const auto contract=endpoint->contract.view();const auto* input=std::get_if<NumericContract>(&contract);
    if(!input)return {ProcessingError::InvalidContract,node.id};
    const auto numerator=std::uint64_t(graph.execution_interval.numerator),denominator=graph.execution_interval.denominator;
    const auto minimum=denominator/numerator+std::uint64_t(denominator%numerator!=0);
    return input->clock.sample_rate>=minimum?ProcessingDiagnostic{}:ProcessingDiagnostic{ProcessingError::InvalidParameters,node.id};
}
}
ProcessingDiagnostic validate_execution_policy(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& node) noexcept {
    const auto cadence=window_cadence(graph,node);if(!cadence)return cadence;
    const auto* mixer=std::get_if<MixerProcessingParameters>(&node.parameters);
    if(node.kind=="Mixer" && mixer && mixer->consumption==MixerConsumption::ClockedFifo && !has_configuration_binding(graph,node.id,ConfigurationRole::SampleClock))return {ProcessingError::InvalidParameters,node.id};
    return requires_alignment(node)?ClockProvenance{graph}.aligned_inputs(node):ProcessingDiagnostic{};
}
}
