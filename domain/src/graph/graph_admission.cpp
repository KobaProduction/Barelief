#include "processing_admission.h"
#include <array>
namespace domain {
namespace {
using namespace signal_contracts;
using namespace detail;
struct Admission {
    const NumericProcessingGraph& graph;
    const ProcessingAdmissionLimits& limits;
    const NumericDeviceCapabilities& capabilities;
    std::span<std::size_t> workspace;
    ProcessingGraphAdmission result{};
    std::size_t metadata=0,workspace_count=0;
    ProcessingDiagnostic header() noexcept;
    ProcessingDiagnostic workspace_budget() noexcept;
    ProcessingDiagnostic nodes() noexcept;
    ProcessingDiagnostic node(std::size_t) noexcept;
    ProcessingDiagnostic edges() noexcept;
    ProcessingDiagnostic order() noexcept;
    ProcessingDiagnostic kernels() noexcept;
    ProcessingDiagnostic conversions() noexcept;
    ProcessingDiagnostic peak() noexcept;
    ProcessingDiagnostic support(const ResolvedProcessingNode&,PortId=0) noexcept;
    ProcessingDiagnostic edge(std::size_t) noexcept;
    ProcessingDiagnostic conversion(const Edge&) noexcept;
    bool duplicate_input(std::size_t) const noexcept;
    void remove_outgoing_degree(NodeId,std::span<std::size_t>) const noexcept;
    bool edge_conversion_needed(const Edge&) const noexcept;
    ProcessingDiagnostic structure() noexcept;
    ProcessingDiagnostic execution() noexcept;
};
ProcessingDiagnostic Admission::header() noexcept {
    if(processing_versions_supported(graph.versions)!=Status::ok)return {ProcessingError::UnsupportedVersion};
    const auto bindings=validate_graph_bindings(graph,limits);if(!bindings)return bindings;
    if(!valid_execution_interval(graph.execution_interval))return {ProcessingError::InvalidParameters};
    if(graph.graph_id.empty() || graph.graph_id.size()>limits.max_text_bytes || graph.nodes.size()>limits.max_nodes || graph.edges.size()>limits.max_edges)return {ProcessingError::Limit};
    return workspace_budget();
}
ProcessingDiagnostic Admission::workspace_budget() noexcept {
    if(!checked_mul(graph.nodes.size(),2,workspace_count))return {ProcessingError::Limit};
    if(workspace.size()<workspace_count)return {ProcessingError::InsufficientWorkspace};
    if(processing_graph_owned_bytes(graph,limits,metadata)!=Status::ok)return {ProcessingError::Limit};
    result.node_count=graph.nodes.size();return {};
}
ProcessingDiagnostic Admission::nodes() noexcept {
    for(std::size_t i=0;i<graph.nodes.size();++i) {
        const auto valid=node(i);if(!valid)return valid;
    }
    return {};
}
ProcessingDiagnostic Admission::node(std::size_t index) noexcept {
    const auto& configured=graph.nodes[index];
    for(std::size_t previous=0;previous<index;++previous)if(graph.nodes[previous].id==configured.id)return {ProcessingError::InvalidNode,configured.id};
    ResolvedProcessingNode resolved;
    const auto valid=resolve_processing_node(configured,limits,resolved);if(!valid)return valid;
    return validate_execution_policy(graph,configured);
}
bool Admission::duplicate_input(std::size_t index) const noexcept {
    const auto& edge=graph.edges[index];
    for(std::size_t previous=0;previous<index;++previous)if(graph.edges[previous].to==edge.to && graph.edges[previous].input_port==edge.input_port)return true;
    return false;
}
ProcessingDiagnostic Admission::edge(std::size_t index) noexcept {
    const auto& edge=graph.edges[index];const auto from=admission_node(graph,edge.from),to=admission_node(graph,edge.to);
    if(from==graph.nodes.size() || to==graph.nodes.size())return {ProcessingError::MissingEndpoint,-1,0,index};
    const auto* source=admission_port(graph.nodes[from].outputs,edge.output_port);const auto* target=admission_port(graph.nodes[to].inputs,edge.input_port);
    if(!source || !target)return {ProcessingError::MissingEndpoint,edge.to,edge.input_port,index};
    if(edge.conversion>3 || (!processing_ports_compatible(*source,*target) && !processing_ports_dtype_convertible(*source,*target)))return {ProcessingError::IncompatiblePorts,edge.to,edge.input_port,index};
    if(duplicate_input(index))return {ProcessingError::MultipleInputs,edge.to,edge.input_port,index};
    return {};
}
ProcessingDiagnostic Admission::edges() noexcept {
    auto degree=workspace.first(graph.nodes.size());std::fill(degree.begin(),degree.end(),0);
    for(std::size_t index=0;index<graph.edges.size();++index) {
        const auto valid=edge(index);if(!valid)return valid;
        if(!checked_add(degree[admission_node(graph,graph.edges[index].to)],1))return {ProcessingError::Limit};
    }
    return {};
}
ProcessingDiagnostic Admission::order() noexcept {
    const auto count=graph.nodes.size();auto degree=workspace.first(count);auto order=workspace.subspan(count,count);
    const auto visited=std::numeric_limits<std::size_t>::max();
    for(std::size_t position=0;position<count;++position) {
        const auto next=std::find(degree.begin(),degree.end(),0);
        if(next==degree.end())return {ProcessingError::Cycle};
        const auto index=std::size_t(next-degree.begin());order[position]=index;*next=visited;
        remove_outgoing_degree(graph.nodes[index].id,degree);
    }
    return {};
}
void Admission::remove_outgoing_degree(NodeId source,std::span<std::size_t> degree) const noexcept {
    const auto visited=std::numeric_limits<std::size_t>::max();
    for(const auto& edge:graph.edges)if(edge.from==source) {
        auto& incoming=degree[admission_node(graph,edge.to)];if(incoming!=visited)--incoming;
    }
}
bool accumulate(ProcessingResources& current,const ProcessingResources& extra,const ProcessingResources& maximum) noexcept {
    if(!checked_add(current.plan_bytes,extra.plan_bytes) || !checked_add(current.payload_bytes,extra.payload_bytes) || !checked_add(current.scratch_bytes,extra.scratch_bytes))return false;
    current.stack_bytes=std::max(current.stack_bytes,extra.stack_bytes);
    return current.plan_bytes<=maximum.plan_bytes && current.payload_bytes<=maximum.payload_bytes && current.scratch_bytes<=maximum.scratch_bytes && current.stack_bytes<=maximum.stack_bytes;
}
ProcessingDiagnostic Admission::support(const ResolvedProcessingNode& node,PortId port) noexcept {
    const auto supported=capabilities.query(capabilities.context,node,capabilities.profile);
    if(!supported.supported)return {ProcessingError::UnsupportedCombination,node.id,port};
    if(!accumulate(result.resources,supported.resources,capabilities.profile.maximum))return {ProcessingError::ResourceLimit,node.id,port};
    return {};
}
ProcessingDiagnostic Admission::kernels() noexcept {
    if(!graph.nodes.empty() && !capabilities.query)return {ProcessingError::UnsupportedCombination};
    for(const auto& node:graph.nodes) {
        ResolvedProcessingNode resolved;const auto valid=resolve_processing_node(node,limits,resolved);if(!valid)return valid;
        const auto budget=support(resolved);if(!budget)return budget;
    }
    return {};
}
ProcessingDiagnostic Admission::conversion(const Edge& edge) noexcept {
    const auto* input=admission_port(graph.nodes[admission_node(graph,edge.from)].outputs,edge.output_port);
    const auto* output=admission_port(graph.nodes[admission_node(graph,edge.to)].inputs,edge.input_port);
    const ProcessingParameters parameters=ConverterProcessingParameters{edge_conversion_policy(edge.conversion)};
    const std::array<ProcessingPort,1> inputs{{{0,PortDirection::Input,input->contract}}},outputs{{{0,PortDirection::Output,output->contract}}};
    const ResolvedProcessingNode node{edge.to,"Converter",NodeRole::Processor,ProcessingOperation::Converter,inputs,outputs,&parameters,true};
    const auto budget=support(node,edge.input_port);if(!budget)return budget;
    if(!checked_add(metadata,sizeof(ConfiguredProcessingNode)+9+2*sizeof(ProcessingPort)+sizeof(Edge)) || !checked_add(metadata,input->contract.owned_bytes()) || !checked_add(metadata,output->contract.owned_bytes()) || metadata>limits.max_owned_metadata_bytes)return {ProcessingError::Limit,edge.to};
    return {};
}
ProcessingDiagnostic Admission::conversions() noexcept {
    std::size_t count=0;
    for(std::size_t index=0;index<graph.edges.size();++index) {
        const auto& edge=graph.edges[index];
        if(!edge_conversion_needed(edge))continue;
        if(++count>limits.max_nodes-graph.nodes.size() || count>limits.max_edges-graph.edges.size())return {ProcessingError::Limit,edge.to,edge.input_port,index};
        auto valid=conversion(edge);if(!valid){valid.edge=index;return valid;}
    }
    result.node_count+=count;return {};
}
bool Admission::edge_conversion_needed(const Edge& edge) const noexcept {
    const auto* source=admission_port(graph.nodes[admission_node(graph,edge.from)].outputs,edge.output_port);
    const auto* target=admission_port(graph.nodes[admission_node(graph,edge.to)].inputs,edge.input_port);
    return !processing_ports_compatible(*source,*target);
}
ProcessingDiagnostic Admission::peak() noexcept {
    auto& peak=result.peak_bytes;peak=capabilities.profile.retained_working_bytes;
    std::size_t workspace_bytes=0;
    if(!checked_mul(workspace_count,sizeof(std::size_t),workspace_bytes))return {ProcessingError::ResourceLimit};
    const auto& resources=result.resources;
    for(const auto bytes:{workspace_bytes,metadata,resources.plan_bytes,resources.payload_bytes,resources.scratch_bytes,resources.stack_bytes})if(!checked_add(peak,bytes))return {ProcessingError::ResourceLimit};
    return peak<=capabilities.profile.max_peak_bytes?ProcessingDiagnostic{}:ProcessingDiagnostic{ProcessingError::ResourceLimit};
}
ProcessingDiagnostic Admission::structure() noexcept {
    auto valid=header();if(!valid)return valid;
    valid=nodes();if(!valid)return valid;
    valid=edges();if(!valid)return valid;
    return order();
}
ProcessingDiagnostic Admission::execution() noexcept {
    auto valid=kernels();if(!valid)return valid;
    valid=conversions();if(!valid)return valid;
    return peak();
}
}
ProcessingDiagnostic admit_processing_graph(const NumericProcessingGraph& graph,const ProcessingAdmissionLimits& limits,
    const NumericDeviceCapabilities& capabilities,std::span<std::size_t> workspace,ProcessingGraphAdmission& out) noexcept {
    Admission admission{graph,limits,capabilities,workspace};
    auto valid=admission.structure();if(!valid)return valid;
    valid=admission.execution();if(!valid)return valid;
    out=admission.result;return {};
}
}
