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
bool parameters_valid(const NodeDefinition& n) {
    if (n.parameters.index()!=default_parameters(n.kind).index()) return false;
    return std::visit([](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T,AudioSourceParameters>) return p.source==AudioSource::SystemOutput || p.source==AudioSource::Microphone;
        else if constexpr (std::is_same_v<T,MergeParameters>) return p.merge>=MergeMode::Mean && p.merge<=MergeMode::RMS;
        else if constexpr (std::is_same_v<T,GainParameters>) return range(p.gain,0,100);
        else if constexpr (std::is_same_v<T,MelParameters>) return p.bands>=1 && p.bands<=50;
        else if constexpr (std::is_same_v<T,HistoryParameters>) return p.history_size>=2 && p.history_size<=2000;
        else if constexpr (std::is_same_v<T,FFTViewParameters>) return p.channel>=Channel::Left && p.channel<=Channel::Merged && p.merge>=MergeMode::Mean && p.merge<=MergeMode::RMS;
        else if constexpr (std::is_same_v<T,WindowParameters>) return p.size>=512 && p.size<=4096 && !(p.size & (p.size-1));
        else if constexpr (std::is_same_v<T,WindowFunctionParameters>) return p.function>=WindowFunction::Hann && p.function<=WindowFunction::Rectangular;
        else if constexpr (std::is_same_v<T,TestSignalParameters>) return p.tones>=5 && p.tones<=10 && p.sample_rate>=8000 && p.sample_rate<=192000 &&
            range(p.minimum_hz,10,96000) && range(p.maximum_hz,10,96000) && p.maximum_hz>p.minimum_hz &&
            range(p.amplitude,0,1) && range(p.period_seconds,.1,3600) && range(p.sweep_depth,0,.95) &&
            p.maximum_hz*(1+p.sweep_depth)<p.sample_rate/2.0;
        else if constexpr (std::is_same_v<T,NetworkParameters>) return p.data_type>=FrameType::PCM && p.data_type<=FrameType::History &&
            p.port>=1 && p.port<=65535 && p.stream_id.find_first_not_of(" \t\r\n\f\v")!=std::string::npos;
        else return true;
    }, n.parameters);
}
}
DeviceCapabilities desktop_capabilities() {
    DeviceCapabilities result;
    for (int i=0;i<19;++i) result.node_kinds.push_back(static_cast<NodeKind>(i));
    for (int i=0;i<7;++i) result.receive_types.push_back(static_cast<FrameType>(i));
    result.publish_types=result.receive_types;
    return result;
}
ValidationResult validate_graph(const GraphDefinition& g, const DeviceCapabilities& caps, GraphLimits limits) {
    auto fail=[](GraphError error, NodeId node=-1, std::size_t edge=0) { return ValidationResult{error,node,edge,{}}; };
    if (g.nodes.empty() || g.nodes.size()>limits.max_nodes || g.edges.size()>limits.max_edges) return fail(GraphError::GraphLimit);
    std::size_t wled=0;
    for (std::size_t i=0;i<g.nodes.size();++i) {
        const auto& n=g.nodes[i];
        if (n.id<0 || n.id==std::numeric_limits<NodeId>::max()) return fail(GraphError::InvalidNodeId,n.id);
        for (std::size_t j=0;j<i;++j) if (n.id==g.nodes[j].id) return fail(GraphError::DuplicateNodeId,n.id);
        if (kind_id(n.kind).empty()) return fail(GraphError::UnknownKind,n.id);
        if (!contains(caps.node_kinds,n.kind)) return fail(GraphError::UnsupportedKind,n.id);
        if (!parameters_valid(n)) return fail(GraphError::InvalidParameters,n.id);
        if (n.kind==NodeKind::WLED && ++wled>caps.max_wled_outputs) return fail(GraphError::OutputLimit,n.id);
        if (n.kind==NodeKind::NetworkInput && !contains(caps.receive_types,*output_type(n))) return fail(GraphError::UnsupportedFrameType,n.id);
        if (n.kind==NodeKind::NetworkOutput && !contains(caps.publish_types,*input_type(n))) return fail(GraphError::UnsupportedFrameType,n.id);
    }
    auto index=[&](NodeId id) { for (std::size_t i=0;i<g.nodes.size();++i) if(g.nodes[i].id==id) return i; return g.nodes.size(); };
    std::vector<std::size_t> parent(g.nodes.size(),g.nodes.size());
    for (std::size_t i=0;i<g.edges.size();++i) {
        const auto& e=g.edges[i]; const auto from=index(e.from),to=index(e.to);
        if (from==g.nodes.size() || to==g.nodes.size() || e.input_port || e.output_port) return fail(GraphError::MissingNodeOrPort,e.to,i);
        const auto out=output_type(g.nodes[from]),in=input_type(g.nodes[to]);
        if (!out || !in || out!=in) return fail(GraphError::IncompatiblePorts,e.to,i);
        if (parent[to]!=g.nodes.size()) return fail(GraphError::MultipleInputs,e.to,i);
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
}
