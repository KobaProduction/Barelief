#pragma once
#include <domain/graph/admission.h>
#include <algorithm>
#include <limits>
namespace domain::detail {
inline bool checked_add(std::size_t& amount,std::size_t extra) noexcept {
    if(extra>std::numeric_limits<std::size_t>::max()-amount)return false;
    amount+=extra;return true;
}
inline bool checked_mul(std::size_t count,std::size_t width,std::size_t& out) noexcept {
    if(count && width>std::numeric_limits<std::size_t>::max()/count)return false;
    out=count*width;return true;
}
inline const ProcessingPort* admission_port(std::span<const ProcessingPort> ports,PortId id) noexcept {
    for(const auto& port:ports)if(port.id==id)return &port;
    return nullptr;
}
inline std::size_t admission_node(const NumericProcessingGraph& graph,NodeId id) noexcept {
    for(std::size_t i=0;i<graph.nodes.size();++i)if(graph.nodes[i].id==id)return i;
    return graph.nodes.size();
}
bool processing_node_owned_bytes(const ConfiguredProcessingNode&,const ProcessingAdmissionLimits&,std::size_t&) noexcept;
ProcessingDiagnostic validate_graph_bindings(const NumericProcessingGraph&,const ProcessingAdmissionLimits&) noexcept;
ProcessingDiagnostic validate_execution_policy(const NumericProcessingGraph&,const ConfiguredProcessingNode&) noexcept;
}
