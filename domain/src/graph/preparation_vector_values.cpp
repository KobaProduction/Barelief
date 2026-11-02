#include <domain/graph/preparation_values.h>
#include <domain/numeric/contracts.h>
#include <domain/signal/contracts.h>
#include <algorithm>
#include <cmath>
#include <limits>
namespace domain {
using namespace signal_contracts;
namespace {
const ConfiguredProcessingNode* find_node(const NumericProcessingGraph& graph,NodeId id) noexcept {
    const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& node){return node.id==id;});
    return found==graph.nodes.end()?nullptr:&*found;
}
const ProcessingPort* find_output(const ConfiguredProcessingNode& node,PortId id) noexcept {
    const auto found=std::find_if(node.outputs.begin(),node.outputs.end(),[=](const auto& port){return port.id==id;});
    return found==node.outputs.end()?nullptr:&*found;
}
Status declared_values(const ConfiguredProcessingNode& node,NumericArray& values) {
    const auto* port=find_output(node,0);if(!port)return Status::invalid_argument;
    const auto data=port->contract.view();const auto* contract=std::get_if<NumericContract>(&data);
    if(!contract || contract->dtype!=DType::Float32 || contract->shape.rank!=Rank::Vector ||
       contract->shape.extents[0].minimum!=contract->shape.extents[0].maximum)return Status::incompatible_contract;
    const auto count=contract->shape.extents[0].maximum;
    if(!count || count>max_numeric_array_values)return Status::exhausted;
    values.values.resize(std::size_t(count));return Status::ok;
}
Status fill_literal(const ConfiguredProcessingNode& node,NumericArray& values) {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ArrayFill,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView parameter;
    if(resolve_processing_parameter(resolved,"value",parameter)!=Status::ok || !std::holds_alternative<NumericLiteral>(parameter))return Status::invalid_argument;
    const auto value=float(std::visit([](auto literal){return double(literal);},std::get<NumericLiteral>(parameter)));
    if(!std::isfinite(value))return Status::overflow;
    std::fill(values.values.begin(),values.values.end(),value);return Status::ok;
}
Status array_fill_values(const ConfiguredProcessingNode& node,NumericArray& values) {
    if(!node.enabled){std::fill(values.values.begin(),values.values.end(),0.f);return Status::ok;}
    std::size_t supplied=0;const auto status=array_generator_values(node,{},supplied);
    if(status!=Status::ok)return status;
    return supplied?array_generator_values(node,values.values,supplied):fill_literal(node,values);
}
Status generated_values(const ConfiguredProcessingNode& node,PortId output,NumericArray& values) {
    if(output!=0 || (node.kind!="WindowFunction" && node.kind!="ArrayFill"))return Status::unavailable;
    const auto status=declared_values(node,values);if(status!=Status::ok)return status;
    return node.kind=="WindowFunction"?window_function_coefficients(node,values.values):array_fill_values(node,values);
}
Status vector_value(const NumericProcessingGraph&,NodeId,PortId,NumericArray&,const PreparationValueProvider*,std::size_t);
Status routed_input(const ConfiguredProcessingNode& node,PortId output,std::uint64_t& input) {
    if(node.kind=="Reroute")return output==0?Status::ok:Status::unavailable;
    if(node.kind!="InputSelector")return Status::unavailable;
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::InputSelector,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView selected;
    if(resolve_processing_parameter(resolved,"selected_input",selected)!=Status::ok || !std::holds_alternative<NumericLiteral>(selected))return Status::invalid_argument;
    const auto group=std::visit([](auto value){return double(value);},std::get<NumericLiteral>(selected));
    const auto outputs=data_outputs(node).size();
    if(!std::isfinite(group) || group<0 || std::floor(group)!=group || group>UINT16_MAX || output>=outputs || group*outputs+output>UINT16_MAX)return Status::invalid_argument;
    input=std::uint64_t(group)*outputs+output;return Status::ok;
}
Status routed_values(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& node,PortId output,
                     NumericArray& values,const PreparationValueProvider* provider,std::size_t depth) {
    std::uint64_t input=0;
    const auto status=routed_input(node,output,input);if(status!=Status::ok)return status;
    const auto edge=std::find_if(graph.edges.begin(),graph.edges.end(),[&](const auto& edge){return edge.to==node.id && edge.input_port==input;});
    return edge==graph.edges.end()?Status::unavailable:vector_value(graph,edge->from,edge->output_port,values,provider,depth+1);
}
Status vector_value(const NumericProcessingGraph& graph,NodeId id,PortId output,NumericArray& values,
                    const PreparationValueProvider* provider,std::size_t depth) {
    if(depth>graph.nodes.size())return Status::invalid_argument;
    if(provider && provider->resolve_vector){
        const auto status=provider->resolve_vector(provider->context,graph,id,output,values);
        if(status!=Status::unavailable)return status;
    }
    const auto* node=find_node(graph,id);if(!node)return Status::invalid_argument;
    const auto capability=preparation_value_capability(*node,output);
    if(capability.kind==PreparationValueKind::Routed)return routed_values(graph,*node,output,values,provider,depth);
    return capability.kind==PreparationValueKind::FiniteVector?generated_values(*node,output,values):Status::unavailable;
}
}
Status resolve_preparation_vector(const NumericProcessingGraph& graph,NodeId id,PortId output,NumericArray& out,
                                  const PreparationValueProvider* provider) {
    NumericArray candidate;const auto status=vector_value(graph,id,output,candidate,provider,0);
    if(status!=Status::ok)return status;
    if(candidate.values.size()>max_numeric_array_values)return Status::exhausted;
    if(!std::all_of(candidate.values.begin(),candidate.values.end(),[](float value){return std::isfinite(value);}))return Status::invalid_argument;
    out=std::move(candidate);return Status::ok;
}
}
