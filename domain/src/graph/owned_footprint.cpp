#include "processing_admission.h"
namespace domain {
namespace {
using namespace signal_contracts;
struct Footprint {
    const ProcessingAdmissionLimits& limits;
    std::size_t bytes;
    bool container(std::size_t count,std::size_t width=1) noexcept {
        std::size_t extra=0;
        return detail::checked_mul(count,width,extra) && detail::checked_add(bytes,extra);
    }
    bool charge(std::size_t count,std::size_t width=1) noexcept {
        return container(count,width) && bytes<=limits.max_owned_metadata_bytes;
    }
    bool text(std::string_view value,std::size_t capacity) noexcept {
        return value.size()<=limits.max_text_bytes && charge(capacity) && charge(1);
    }
    bool field(const ProcessingParameter& parameter) noexcept {
        if(!text(parameter.key,parameter.key.capacity()))return false;
        if(const auto* value=std::get_if<std::string>(&parameter.value))return text(*value,value->capacity());
        if(const auto* value=std::get_if<NumericArray>(&parameter.value))return charge(value->values.capacity(),sizeof(float));
        return true;
    }
    bool fields(const std::vector<ProcessingParameter>& values) noexcept {
        if(values.size()>limits.max_parameters_per_node || !charge(values.capacity(),sizeof(ProcessingParameter)))return false;
        for(const auto& parameter:values)if(!field(parameter))return false;
        return true;
    }
    bool ports(const std::vector<ProcessingPort>& values) noexcept {
        if(!charge(values.capacity(),sizeof(ProcessingPort)))return false;
        for(const auto& port:values)if(!charge(port.contract.owned_bytes()))return false;
        return true;
    }
    bool parameters(const ProcessingParameters& parameters) noexcept;
    bool stateful_parameters(const ProcessingParameters& parameters) noexcept;
};
bool Footprint::parameters(const ProcessingParameters& parameters) noexcept {
    if(const auto* value=std::get_if<EndpointProcessingParameters>(&parameters))return fields(value->values);
    if(const auto* value=std::get_if<TransformProcessingParameters>(&parameters))return fields(value->values);
    return stateful_parameters(parameters);
}
bool Footprint::stateful_parameters(const ProcessingParameters& parameters) noexcept {
    if(const auto* value=std::get_if<SignalGeneratorProcessingParameters>(&parameters))return charge(value->oscillators.capacity(),sizeof(OscillatorParameters));
    if(const auto* value=std::get_if<MixerProcessingParameters>(&parameters))return charge(value->weights.capacity(),sizeof(MixerInputWeight));
    if(const auto* value=std::get_if<SampleClockProcessingParameters>(&parameters))return text(value->domain,value->domain.capacity());
    if(const auto* value=std::get_if<ReduceProcessingParameters>(&parameters))return value->selected_channels.size()<=limits.frame.max_planes && charge(value->selected_channels.capacity(),sizeof(std::uint32_t));
    return true;
}
bool graph_container_bytes(const NumericProcessingGraph& graph,Footprint& footprint) noexcept {
    return footprint.container(graph.nodes.capacity()-graph.nodes.size(),sizeof(ConfiguredProcessingNode)) &&
        footprint.container(graph.configuration_bindings.capacity(),sizeof(ConfigurationBinding)) &&
        footprint.container(graph.edges.capacity(),sizeof(Edge)) && footprint.container(graph.graph_id.capacity()) && footprint.container(1);
}
}
namespace detail {
bool processing_node_owned_bytes(const ConfiguredProcessingNode& node,const ProcessingAdmissionLimits& limits,std::size_t& out) noexcept {
    Footprint footprint{limits,sizeof(ConfiguredProcessingNode)};
    if(!footprint.charge(node.kind.capacity()) || !footprint.charge(1) || !footprint.ports(node.inputs) || !footprint.ports(node.outputs) || !footprint.parameters(node.parameters))return false;
    out=footprint.bytes;return true;
}
}
signal_contracts::Status processing_graph_owned_bytes(const NumericProcessingGraph& graph,const ProcessingAdmissionLimits& limits,std::size_t& out) noexcept {
    if(graph.nodes.size()>limits.max_nodes || graph.edges.size()>limits.max_edges)return signal_contracts::Status::exhausted;
    Footprint footprint{limits,sizeof(NumericProcessingGraph)};
    if(!graph_container_bytes(graph,footprint))return signal_contracts::Status::overflow;
    if(footprint.bytes>limits.max_owned_metadata_bytes)return signal_contracts::Status::exhausted;
    for(const auto& node:graph.nodes) {
        std::size_t bytes=0;
        if(!detail::processing_node_owned_bytes(node,limits,bytes) || !footprint.charge(bytes))return signal_contracts::Status::exhausted;
    }
    out=footprint.bytes;return signal_contracts::Status::ok;
}
}
