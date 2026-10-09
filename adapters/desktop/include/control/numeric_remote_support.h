#pragma once
#include <control/numeric_remote_wire.h>
#include <control/numeric_processing_message_mapper.h>
#include <desktop/numeric_project_document.h>
#include <QJsonDocument>
#include <chrono>
namespace pipeline::remote_numeric {
struct Clock final : application::ControlClock {
    std::uint64_t now_ms() const noexcept override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};
inline nc::Limits control_limits() {
    auto l=nc::desktop_limits();l.nodes=512;l.edges=2048;l.ports=8192;l.parameters=16384;l.records=65536;l.capability_rows=512;return l;
}
inline const domain::ProcessingPort* find_port(const domain::NumericProcessingGraph& g,const EndpointIdentity& e) {
    if(g.graph_id!=e.graph_id)return nullptr;
    for(const auto& n:g.nodes)if(n.id==e.node) {
        const auto& ports=e.direction==domain::PortDirection::Input?n.inputs:n.outputs;
        for(const auto& p:ports)if(p.id==e.port)return &p;
    }
    return nullptr;
}
inline EndpointIdentity endpoint(const nd::Endpoint& e) {
    return {std::string(e.graph),e.node,e.direction==nd::Direction::Input?domain::PortDirection::Input:domain::PortDirection::Output,e.port};
}
inline const DeliveredNumericFrame& frame(const Queued& q) {
    return std::visit([](const auto& d)->const DeliveredNumericFrame& {
        if constexpr(std::is_same_v<std::decay_t<decltype(d)>,DeliveredNumericFrame>)return d;else return d.compact;
    },q);
}
inline bool project_graph(const domain::NumericProcessingGraph& g,QJsonObject& out) {
    QByteArray bytes;if(!desktop::encode_numeric_graph(g,bytes))return false;
    out=QJsonDocument::fromJson(bytes).object();return true;
}
inline std::vector<EndpointIdentity> default_endpoints(const domain::NumericProcessingGraph& g) {
    std::vector<EndpointIdentity> endpoints;
    for(const auto& n:g.nodes) {
        for(const auto& p:n.outputs)endpoints.push_back({g.graph_id,n.id,domain::PortDirection::Output,p.id});
        if(n.outputs.empty())for(const auto& p:n.inputs)endpoints.push_back({g.graph_id,n.id,domain::PortDirection::Input,p.id});
    }
    return endpoints;
}
struct CapabilitySnapshot {
    control::numeric::GraphProjection graph;
    std::vector<nc::CapabilityRow> rows;
    nc::Capabilities value;
    Status assign(const domain::NumericProcessingGraph& g,const application::ProcessingCapabilities& profile) {
        if(!profile.numeric)return Status::invalid_argument;
        const auto& p=*profile.numeric;const auto& maximum=p.runtime.resources;
        value.maximum={maximum.maximum.plan_bytes,maximum.maximum.payload_bytes,maximum.maximum.scratch_bytes,
            maximum.maximum.stack_bytes,maximum.max_peak_bytes};
        if(!g.nodes.empty()) {
            if(!graph.assign(g,control_limits()))return Status::invalid_argument;
            rows.reserve(g.nodes.size());
            for(std::size_t i=0;i<g.nodes.size();++i) {
                domain::ResolvedProcessingNode resolved;
                if(!domain::resolve_processing_node(g.nodes[i],p.runtime.admission,resolved))return Status::invalid_argument;
                auto support=p.device.query(p.device.context,resolved,maximum);if(!support.supported)return Status::incompatible_contract;
                const auto r=support.resources;
                rows.push_back({graph.wire().nodes[i],graph.wire().nodes[i].kind,"desktop-cpu",signal_contracts::ValueDomain::FiniteOnly,
                    {r.plan_bytes,r.payload_bytes,r.scratch_bytes,r.stack_bytes,r.plan_bytes+r.payload_bytes+r.scratch_bytes+r.stack_bytes}});
            }
        }
        value.rows=rows;return Status::ok;
    }
};
}
