#include "example_builder.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace examples {
namespace {
void require(domain::ProcessingDiagnostic result,std::string_view operation={}) {
    if(!result)throw std::runtime_error("Example contract: node="+std::to_string(result.node)+
        " error="+std::to_string(unsigned(result.error))+" operation="+std::string(operation));
}
std::vector<domain::ProcessingParameter>* parameter_fields(domain::ProcessingParameters& parameters) {
    if(auto* fields=std::get_if<domain::EndpointProcessingParameters>(&parameters))return &fields->values;
    if(auto* fields=std::get_if<domain::TransformProcessingParameters>(&parameters))return &fields->values;
    return nullptr;
}
void apply_parameters(domain::ConfiguredProcessingNode& node,domain::ProcessingParameters parameters) {
    auto* destination=parameter_fields(node.parameters);
    auto* source=parameter_fields(parameters);
    if(!destination || !source){node.parameters=std::move(parameters);return;}
    for(auto& field:*source) {
        auto existing=std::find_if(destination->begin(),destination->end(),[&](const auto& p){return p.key==field.key;});
        if(existing==destination->end())destination->push_back(std::move(field));
        else existing->value=std::move(field.value);
    }
}
}
Builder::Builder(domain::NodeFactorySettings settings)
    : limits{512,2048,64,128,1048576,16777216,{16,800000,16777216,65536,4096,65536}},settings_(settings) {
    graph.graph_id="main";
    graph.execution_interval={1,120};
}
domain::ConfiguredProcessingNode& Builder::node(NodeId id) {
    auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& n){return n.id==id;});
    if(found==graph.nodes.end())throw std::runtime_error("Missing example node "+std::to_string(id));
    return *found;
}
const domain::OwnedProcessingContract& Builder::output(NodeId id,PortId port) {
    const auto& ports=node(id).outputs;
    auto found=std::find_if(ports.begin(),ports.end(),[=](const auto& p){return p.id==port;});
    if(found==ports.end())throw std::runtime_error("Missing example output");
    return found->contract;
}
void Builder::add(NodeId id,std::string_view kind,std::optional<domain::ProcessingParameters> parameters) {
    domain::ConfiguredProcessingNode value;
    require(domain::make_processing_node(kind,id,limits,value,settings_),"create "+std::string(kind));
    if(parameters)apply_parameters(value,std::move(*parameters));
    graph.nodes.push_back(std::move(value));
}
void Builder::chain(NodeId id,std::string_view kind,NodeId from,
    std::optional<domain::ProcessingParameters> parameters,PortId port) {
    add(id,kind,std::move(parameters));
    const auto input=output(from,port);
    require(domain::configure_processing_input(node(id),input.view(),limits),"configure "+std::string(kind));
    connect(from,id,port);
}
void Builder::connect(NodeId from,NodeId to,PortId out,PortId in) {
    const auto contract=output(from,out);
    auto& inputs=node(to).inputs;
    auto found=std::find_if(inputs.begin(),inputs.end(),[=](const auto& p){return p.id==in;});
    if(found==inputs.end())inputs.push_back({in,domain::PortDirection::Input,contract});
    else found->contract=contract;
    graph.edges.push_back({from,to,out,in});
}
void Builder::derive(NodeId id) {require(domain::derive_processing_node_contracts(node(id),limits));}
void Builder::number(NodeId id,double value,signal_contracts::NumericUnit unit) {
    add(id,"Number",domain::EndpointProcessingParameters{{{"value",signal_contracts::NumericLiteral{value}}}});
    auto data=output(id).view();
    auto& scalar=std::get<signal_contracts::NumericContract>(data);
    scalar.dtype=signal_contracts::DType::Float64;
    scalar.representation.alignment=alignof(double);
    scalar.unit=unit;
    domain::OwnedProcessingContract contract;
    if(domain::own_processing_contract(scalar,limits.frame,limits.max_owned_metadata_bytes,contract)!=signal_contracts::Status::ok)
        throw std::runtime_error("Example Float64 constant contract");
    node(id).outputs[0].contract=std::move(contract);
}
void Builder::configuration(NodeId from,NodeId to) {
    graph.configuration_bindings.push_back({from,to});
}
domain::NumericProcessingGraph Builder::finish() {
    for(auto& value:graph.nodes)require(domain::attach_numeric_parameter_ports(value,limits)==signal_contracts::Status::ok?
        domain::ProcessingDiagnostic{}:domain::ProcessingDiagnostic{domain::ProcessingError::InvalidPort,value.id});
    domain::NumericProcessingGraph evaluated,executable;
    require(domain::resolve_numeric_parameter_bindings(graph,limits,evaluated,executable),"resolve example bindings");
    return evaluated;
}
domain::TransformProcessingParameters transform(std::initializer_list<domain::ProcessingParameter> fields) {
    return {std::vector<domain::ProcessingParameter>(fields)};
}
void add_window(Builder& b,NodeId source,NodeId buffer,NodeId coefficients,NodeId length,NodeId product,std::uint64_t size,PortId source_port) {
    using signal_contracts::NumericLiteral;
    b.chain(buffer,"Window",source,transform({{"size",NumericLiteral{size}}}),source_port);
    b.chain(length,"ArrayLength",buffer);
    b.chain(coefficients,"WindowFunction",length,transform({{"size",NumericLiteral{size}},{"function",std::string{"hann"}}}));
    b.add(product,"ArrayMultiply");
    b.connect(buffer,product);
    b.connect(coefficients,product,0,1);
    b.derive(product);
}
void add_frequency_step(Builder& b,NodeId source,NodeId length,NodeId step) {
    b.chain(step,"FrequencyStep",source,{},domain::sample_rate_output);
    b.connect(length,step,0,domain::window_size_input);
}
}
