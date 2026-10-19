#include <domain/graph_validation.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
namespace domain {
namespace {
template<class T> bool contains(const std::vector<T>& values, T v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}
bool range(double v, double lo, double hi) { return std::isfinite(v) && v>=lo && v<=hi; }
bool within(NodeKind kind,std::string_view key,double value) noexcept {
    const auto* descriptor=parameter_descriptor(kind,key);
    return descriptor && range(value,descriptor->minimum,descriptor->maximum);
}
std::string_view checked(std::string_view key,bool valid) noexcept {return valid?std::string_view{}:key;}
bool merge_valid(MergeMode mode) noexcept {
    return mode==MergeMode::Mean || mode==MergeMode::Maximum || mode==MergeMode::RMS;
}
template<class T> std::string_view parameter_error(NodeKind,const T&) noexcept {return {};}
std::string_view parameter_error(NodeKind,const AudioSourceParameters& p) noexcept {
    return checked("source",p.source==AudioSource::SystemOutput || p.source==AudioSource::Microphone);
}
std::string_view parameter_error(NodeKind,const MergeParameters& p) noexcept {return checked("merge",merge_valid(p.merge));}
std::string_view parameter_error(NodeKind kind,const GainParameters& p) noexcept {return checked("gain",within(kind,"gain",p.gain));}
std::string_view parameter_error(NodeKind kind,const MelParameters& p) noexcept {return checked("bands",within(kind,"bands",p.bands));}
std::string_view parameter_error(NodeKind kind,const HistoryParameters& p) noexcept {return checked(kind==NodeKind::NumberAccumulator?"capacity":"history_size",within(kind,kind==NodeKind::NumberAccumulator?"capacity":"history_size",p.history_size));}
std::string_view parameter_error(NodeKind,const FFTViewParameters& p) noexcept {
    if(p.channel!=Channel::Left && p.channel!=Channel::Right && p.channel!=Channel::Merged)return "channel";
    return checked("merge",merge_valid(p.merge));
}
std::string_view parameter_error(NodeKind kind,const WindowParameters& p) noexcept {
    return checked("size",within(kind,"size",p.size) && !(p.size & (p.size-1)));
}
std::string_view parameter_error(NodeKind,const WindowFunctionParameters& p) noexcept {
    return checked("function",p.function>=WindowFunction::Hann && p.function<=WindowFunction::FlatTop);
}
std::string_view test_signal_range_error(NodeKind kind,const TestSignalParameters& p) noexcept {
    if(!within(kind,"tones",p.tones))return "tones";
    if(!within(kind,"sample_rate",p.sample_rate))return "sample_rate";
    if(!within(kind,"minimum_hz",p.minimum_hz))return "minimum_hz";
    if(!within(kind,"maximum_hz",p.maximum_hz) || p.maximum_hz<=p.minimum_hz)return "maximum_hz";
    return {};
}

std::string_view parameter_error(NodeKind kind,const TestSignalParameters& p) noexcept {
    if(const auto error=test_signal_range_error(kind,p);!error.empty())return error;
    if(!within(kind,"amplitude",p.amplitude))return "amplitude";
    if(!within(kind,"period_seconds",p.period_seconds))return "period_seconds";
    if(!within(kind,"sweep_depth",p.sweep_depth))return "sweep_depth";
    return checked("maximum_hz",p.maximum_hz*(1+p.sweep_depth)<p.sample_rate/2.0);
}
std::string_view parameter_error(NodeKind kind,const NetworkParameters& p) noexcept {
    if(!signal_contracts::frame_descriptor(p.data_type))return "data_type";
    if(!within(kind,"port",p.port))return "port";
    return checked("stream_id",p.stream_id.find_first_not_of(" \t\r\n\f\v")!=std::string::npos);
}
std::string_view parameter_error(NodeKind,const RoutingParameters& p) noexcept {
    if(!signal_contracts::frame_descriptor(p.data_type))return "data_type";
    if(p.outputs<2 || p.outputs>8)return "outputs";
    return checked("selected",p.selected<p.outputs);
}
std::string_view parameter_error(NodeKind,const ReductionParameters& p) noexcept {
    if(!signal_contracts::frame_descriptor(p.data_type) || p.data_type==FrameType::WledPacket)return "data_type";
    return checked("operation",p.operation==Reduction::Maximum || p.operation==Reduction::Mean || p.operation==Reduction::RMS);
}
ValidationResult failure(GraphError error,NodeId node=-1,std::optional<std::size_t> edge={},
                         std::string_view key={},std::optional<PortDirection> direction={},
                         std::optional<std::uint16_t> port={}) {
    return {error,node,edge.value_or(0),{},{node,key,direction,port,edge}};
}
ValidationResult validate_node_identity(const GraphDefinition& graph,std::size_t index) {
    const auto& node=graph.nodes[index];
    if(node.id<0 || node.id==std::numeric_limits<NodeId>::max())return failure(GraphError::InvalidNodeId,node.id);
    for(std::size_t prior=0;prior<index;++prior)
        if(node.id==graph.nodes[prior].id)return failure(GraphError::DuplicateNodeId,node.id);
    if(kind_id(node.kind).empty())return failure(GraphError::UnknownKind,node.id);
    return {};
}
ValidationResult validate_node_capability(const NodeDefinition& node,const DeviceCapabilities& caps,
                                          std::size_t& wled) {
    if(!contains(caps.node_kinds,node.kind))return failure(GraphError::UnsupportedKind,node.id);
    const auto parameters=validate_parameters(node);
    if(!parameters.valid)return failure(GraphError::InvalidParameters,node.id,{},parameters.parameter_key);
    if((node.kind==NodeKind::WLED || node.kind==NodeKind::WLEDOutput) &&
       ++wled>caps.max_wled_outputs)return failure(GraphError::OutputLimit,node.id);
    return {};
}
ValidationResult validate_node_transport(const NodeDefinition& node,const DeviceCapabilities& caps) {
    if(node.kind==NodeKind::NetworkInput && !contains(caps.receive_types,*output_type(node)))
        return failure(GraphError::UnsupportedFrameType,node.id,{},"data_type",PortDirection::Output,0);
    if(node.kind==NodeKind::NetworkOutput && !contains(caps.publish_types,*input_type(node)))
        return failure(GraphError::UnsupportedFrameType,node.id,{},"data_type",PortDirection::Input,0);
    return {};
}
ValidationResult validate_nodes(const GraphDefinition& graph,const DeviceCapabilities& caps) {
    std::size_t wled=0;
    for(std::size_t index=0;index<graph.nodes.size();++index) {
        const auto identity=validate_node_identity(graph,index);if(!identity)return identity;
        const auto capability=validate_node_capability(graph.nodes[index],caps,wled);if(!capability)return capability;
        const auto transport=validate_node_transport(graph.nodes[index],caps);if(!transport)return transport;
    }
    return {};
}
std::size_t node_index(const GraphDefinition& graph,NodeId id) noexcept {
    for(std::size_t index=0;index<graph.nodes.size();++index)
        if(graph.nodes[index].id==id)return index;
    return graph.nodes.size();
}
ValidationResult validate_edge_endpoints(const GraphDefinition& graph,const Edge& edge,
                                         std::size_t index,std::size_t from,std::size_t to) {
    if(from==graph.nodes.size())return failure(GraphError::MissingNodeOrPort,edge.from,index,{},PortDirection::Output,edge.output_port);
    if(to==graph.nodes.size())return failure(GraphError::MissingNodeOrPort,edge.to,index,{},PortDirection::Input,edge.input_port);
    if(edge.output_port>=output_ports(graph.nodes[from]))return failure(GraphError::MissingNodeOrPort,edge.from,index,{},PortDirection::Output,edge.output_port);
    if(edge.input_port>=input_ports(graph.nodes[to]))return failure(GraphError::MissingNodeOrPort,edge.to,index,{},PortDirection::Input,edge.input_port);
    return {};
}

ValidationResult validate_edge(const GraphDefinition& graph,const Edge& edge,std::size_t index,
                               std::vector<std::size_t>& parent) {
    const auto from=node_index(graph,edge.from),to=node_index(graph,edge.to);
    const auto endpoints=validate_edge_endpoints(graph,edge,index,from,to);
    if(!endpoints)return endpoints;
    const auto output=output_type(graph.nodes[from],edge.output_port);
    const auto input=input_type(graph.nodes[to],edge.input_port);
    if(!output || !input || output!=input)return failure(GraphError::IncompatiblePorts,edge.to,index,{},PortDirection::Input,edge.input_port);
    if(parent[to]!=graph.nodes.size())return failure(GraphError::MultipleInputs,edge.to,index,{},PortDirection::Input,edge.input_port);
    parent[to]=from;
    return {};
}
ValidationResult validate_edges(const GraphDefinition& graph,std::vector<std::size_t>& parent) {
    for(std::size_t index=0;index<graph.edges.size();++index) {
        const auto result=validate_edge(graph,graph.edges[index],index,parent);
        if(!result)return result;
    }
    return {};
}
ValidationResult visit_dependency(const GraphDefinition& graph,const std::vector<std::size_t>& parent,
                                  std::vector<unsigned char>& marks,std::vector<std::size_t>& path,
                                  std::size_t root,ValidationResult& result) {
    path.clear();auto at=root;
    while(at<graph.nodes.size() && marks[at]==0){marks[at]=1;path.push_back(at);at=parent[at];}
    if(at<graph.nodes.size() && marks[at]==1)return failure(GraphError::Cycle,graph.nodes[at].id);
    while(!path.empty()) {
        const auto node=path.back();path.pop_back();marks[node]=2;
        result.topological_order.push_back(graph.nodes[node].id);
    }
    return {};
}
ValidationResult order_nodes(const GraphDefinition& graph,const std::vector<std::size_t>& parent) {
    std::vector<std::size_t> sorted(graph.nodes.size()),path;
    std::iota(sorted.begin(),sorted.end(),0);
    std::sort(sorted.begin(),sorted.end(),[&](auto a,auto b){return graph.nodes[a].id<graph.nodes[b].id;});
    std::vector<unsigned char> marks(graph.nodes.size(),0);
    ValidationResult result;
    for(auto root:sorted) {
        const auto status=visit_dependency(graph,parent,marks,path,root,result);
        if(!status)return status;
    }
    return result;
}
}
ParameterValidationResult validate_parameters(const NodeDefinition& n) noexcept {
    return validate_parameters(n.kind,n.parameters);
}
ParameterValidationResult validate_parameters(NodeKind kind,const NodeParameters& parameters) noexcept {
    if(!node_descriptor(kind))return {};
    if(is_view(kind) && std::holds_alternative<NoParameters>(parameters))return {true,{}};
    if (parameters.index()!=default_parameters(kind).index()) return {};
    const auto key=std::visit([&](const auto& p)->std::string_view {
        return parameter_error(kind,p);
    }, parameters);
    // Legacy unused fields may still be invalid, but must not masquerade as
    // registered processing keys of a different configured node.
    return {key.empty(),parameter_descriptor(kind,key)?key:std::string_view{}};
}
bool parameters_valid(const NodeDefinition& n) noexcept {
    return validate_parameters(n).valid;
}
DeviceCapabilities desktop_capabilities() {
    DeviceCapabilities result;
    for (const auto& d:node_descriptors()) result.node_kinds.push_back(d.kind);
    for (const auto& d:signal_contracts::frame_descriptors()) result.receive_types.push_back(d.type);
    result.publish_types=result.receive_types;
    return result;
}
ValidationResult validate_graph(const GraphDefinition& g, const DeviceCapabilities& caps, GraphLimits limits) {
    if(!g.processing_version || g.processing_version!=caps.processing_version)
        return failure(GraphError::UnsupportedVersion,-1,{},"processing_version");
    if(g.nodes.empty() || g.nodes.size()>limits.max_nodes || g.edges.size()>limits.max_edges)
        return failure(GraphError::GraphLimit);
    const auto nodes=validate_nodes(g,caps);if(!nodes)return nodes;
    std::vector<std::size_t> parent(g.nodes.size(),g.nodes.size());
    const auto edges=validate_edges(g,parent);if(!edges)return edges;
    // Iterative dependency walk avoids recursion on embedded stacks. Sorted IDs
    // retain the previous compiler's deterministic order for independent nodes.
    return order_nodes(g,parent);
}
ValidationResult validate_processing_graph(const GraphDefinition& g,const DeviceCapabilities& caps,GraphLimits limits) {
    for(const auto& n:g.nodes)
        if(is_view(n.kind) && !std::holds_alternative<NoParameters>(n.parameters))
            return {GraphError::InvalidParameters,n.id,0,{},
                {n.id,n.kind==NodeKind::FFTView && std::holds_alternative<FFTViewParameters>(n.parameters)?std::string_view{"channel"}:std::string_view{},{},{},{}}};
    return validate_graph(g,caps,limits);
}
ValidationResult validate_processing_parameter_update(const GraphDefinition& graph,NodeId id,const NodeParameters& parameters) noexcept {
    const auto it=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==id;});
    if(it==graph.nodes.end())return {GraphError::MissingNodeOrPort,id,0,{},{id,{},{},{},{}}};
    if(is_view(it->kind))return {GraphError::InvalidParameters,id,0,{},
        {id,it->kind==NodeKind::FFTView && std::holds_alternative<FFTViewParameters>(parameters)?std::string_view{"channel"}:std::string_view{},{},{},{}}};
    const auto result=validate_parameters(it->kind,parameters);
    if(!result.valid)return {GraphError::InvalidParameters,id,0,{},{id,result.parameter_key,{},{},{}}};
    return {};
}
}
