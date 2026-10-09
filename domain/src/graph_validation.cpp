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
}
ParameterValidationResult validate_parameters(const NodeDefinition& n) noexcept {
    return validate_parameters(n.kind,n.parameters);
}
ParameterValidationResult validate_parameters(NodeKind kind,const NodeParameters& parameters) noexcept {
    if(!node_descriptor(kind))return {};
    if(is_view(kind) && std::holds_alternative<NoParameters>(parameters))return {true,{}};
    if (parameters.index()!=default_parameters(kind).index()) return {};
    auto within=[&](std::string_view key,double value) {
        const auto* d=parameter_descriptor(kind,key);
        return d && range(value,d->minimum,d->maximum);
    };
    auto merge_valid=[](MergeMode m) { return m==MergeMode::Mean || m==MergeMode::Maximum || m==MergeMode::RMS; };
    auto checked=[](std::string_view key,bool valid) { return valid?std::string_view{}:key; };
    const auto key=std::visit([&](const auto& p)->std::string_view {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T,AudioSourceParameters>) return checked("source",p.source==AudioSource::SystemOutput || p.source==AudioSource::Microphone);
        else if constexpr (std::is_same_v<T,MergeParameters>) return checked("merge",merge_valid(p.merge));
        else if constexpr (std::is_same_v<T,GainParameters>) return checked("gain",within("gain",p.gain));
        else if constexpr (std::is_same_v<T,MelParameters>) return checked("bands",within("bands",p.bands));
        else if constexpr (std::is_same_v<T,HistoryParameters>) return checked("history_size",within("history_size",p.history_size));
        else if constexpr (std::is_same_v<T,FFTViewParameters>) {
            if(p.channel!=Channel::Left && p.channel!=Channel::Right && p.channel!=Channel::Merged)return "channel";
            return checked("merge",merge_valid(p.merge));
        } else if constexpr (std::is_same_v<T,WindowParameters>) return checked("size",within("size",p.size) && !(p.size & (p.size-1)));
        else if constexpr (std::is_same_v<T,WindowFunctionParameters>) return checked("function",p.function==WindowFunction::Hann || p.function==WindowFunction::Hamming || p.function==WindowFunction::Blackman || p.function==WindowFunction::Rectangular);
        else if constexpr (std::is_same_v<T,TestSignalParameters>) {
            if(!within("tones",p.tones))return "tones";
            if(!within("sample_rate",p.sample_rate))return "sample_rate";
            if(!within("minimum_hz",p.minimum_hz))return "minimum_hz";
            if(!within("maximum_hz",p.maximum_hz) || p.maximum_hz<=p.minimum_hz)return "maximum_hz";
            if(!within("amplitude",p.amplitude))return "amplitude";
            if(!within("period_seconds",p.period_seconds))return "period_seconds";
            if(!within("sweep_depth",p.sweep_depth))return "sweep_depth";
            return checked("maximum_hz",p.maximum_hz*(1+p.sweep_depth)<p.sample_rate/2.0);
        } else if constexpr (std::is_same_v<T,NetworkParameters>) {
            if(!signal_contracts::frame_descriptor(p.data_type))return "data_type";
            if(!within("port",p.port))return "port";
            return checked("stream_id",p.stream_id.find_first_not_of(" \t\r\n\f\v")!=std::string::npos);
        } else if constexpr (std::is_same_v<T,RoutingParameters>) {
            if(!signal_contracts::frame_descriptor(p.data_type))return "data_type";
            if(p.outputs<2 || p.outputs>8)return "outputs";
            return checked("selected",p.selected<p.outputs);
        } else if constexpr (std::is_same_v<T,ReductionParameters>) {
            if(!signal_contracts::frame_descriptor(p.data_type) || p.data_type==FrameType::WledPacket)return "data_type";
            return checked("operation",p.operation==Reduction::Maximum || p.operation==Reduction::Mean || p.operation==Reduction::RMS);
        } else return {};
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
    auto fail=[](GraphError error, NodeId node=-1, std::optional<std::size_t> edge={},
                 std::string_view key={},std::optional<PortDirection> direction={},std::optional<std::uint16_t> port={}) {
        return ValidationResult{error,node,edge.value_or(0),{},{node,key,direction,port,edge}};
    };
    if(!g.processing_version || g.processing_version!=caps.processing_version)return fail(GraphError::UnsupportedVersion,-1,{},"processing_version");
    if (g.nodes.empty() || g.nodes.size()>limits.max_nodes || g.edges.size()>limits.max_edges) return fail(GraphError::GraphLimit);
    std::size_t wled=0;
    for (std::size_t i=0;i<g.nodes.size();++i) {
        const auto& n=g.nodes[i];
        if (n.id<0 || n.id==std::numeric_limits<NodeId>::max()) return fail(GraphError::InvalidNodeId,n.id);
        for (std::size_t j=0;j<i;++j) if (n.id==g.nodes[j].id) return fail(GraphError::DuplicateNodeId,n.id);
        if (kind_id(n.kind).empty()) return fail(GraphError::UnknownKind,n.id);
        if (!contains(caps.node_kinds,n.kind)) return fail(GraphError::UnsupportedKind,n.id);
        const auto parameters=validate_parameters(n);
        if (!parameters.valid) return fail(GraphError::InvalidParameters,n.id,{},parameters.parameter_key);
        if ((n.kind==NodeKind::WLED || n.kind==NodeKind::WLEDOutput) && ++wled>caps.max_wled_outputs) return fail(GraphError::OutputLimit,n.id);
        if (n.kind==NodeKind::NetworkInput && !contains(caps.receive_types,*output_type(n))) return fail(GraphError::UnsupportedFrameType,n.id,{},"data_type",PortDirection::Output,0);
        if (n.kind==NodeKind::NetworkOutput && !contains(caps.publish_types,*input_type(n))) return fail(GraphError::UnsupportedFrameType,n.id,{},"data_type",PortDirection::Input,0);
    }
    auto index=[&](NodeId id) { for (std::size_t i=0;i<g.nodes.size();++i) if(g.nodes[i].id==id) return i; return g.nodes.size(); };
    std::vector<std::size_t> parent(g.nodes.size(),g.nodes.size());
    for (std::size_t i=0;i<g.edges.size();++i) {
        const auto& e=g.edges[i]; const auto from=index(e.from),to=index(e.to);
        if(from==g.nodes.size())return fail(GraphError::MissingNodeOrPort,e.from,i,{},PortDirection::Output,e.output_port);
        if(to==g.nodes.size())return fail(GraphError::MissingNodeOrPort,e.to,i,{},PortDirection::Input,e.input_port);
        if(e.output_port>=output_ports(g.nodes[from]))return fail(GraphError::MissingNodeOrPort,e.from,i,{},PortDirection::Output,e.output_port);
        if(e.input_port>=input_ports(g.nodes[to]))return fail(GraphError::MissingNodeOrPort,e.to,i,{},PortDirection::Input,e.input_port);
        const auto out=output_type(g.nodes[from],e.output_port),in=input_type(g.nodes[to],e.input_port);
        if (!out || !in || out!=in) return fail(GraphError::IncompatiblePorts,e.to,i,{},PortDirection::Input,e.input_port);
        if (parent[to]!=g.nodes.size()) return fail(GraphError::MultipleInputs,e.to,i,{},PortDirection::Input,e.input_port);
        parent[to]=from;
    }
    // Iterative dependency walk avoids recursion on embedded stacks. Sorted IDs
    // retain the previous compiler's deterministic order for independent nodes.
    std::vector<std::size_t> sorted(g.nodes.size()),path;
    std::iota(sorted.begin(),sorted.end(),0);
    std::sort(sorted.begin(),sorted.end(),[&](auto a,auto b){return g.nodes[a].id<g.nodes[b].id;});
    std::vector<unsigned char> marks(g.nodes.size(),0);
    ValidationResult result;
    for(auto root:sorted) {
        path.clear(); auto at=root;
        while(at<g.nodes.size() && marks[at]==0) { marks[at]=1;path.push_back(at);at=parent[at]; }
        if(at<g.nodes.size() && marks[at]==1) return fail(GraphError::Cycle,g.nodes[at].id);
        while(!path.empty()) { const auto n=path.back();path.pop_back();marks[n]=2;result.topological_order.push_back(g.nodes[n].id); }
    }
    return result;
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
