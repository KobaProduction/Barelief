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
inline constexpr std::size_t ControlMaximumPeers=4;
inline constexpr std::size_t ControlPeerBudgetBytes=13*1024*1024;
inline constexpr std::size_t ControlStorageBudgetBytes=17*1024*1024;
// Four bounded session allocations plus their conservative in-flight/OS byte
// ceilings are reserved by the local desktop runtime, separately from replay.
inline constexpr std::size_t ControlTransportReservationBytes=ControlMaximumPeers*(ControlPeerBudgetBytes+ControlStorageBudgetBytes);
inline nc::Limits control_limits() {
    auto l=nc::desktop_limits();l.nodes=512;l.edges=2048;l.ports=8192;l.parameters=16384;l.records=65536;l.capability_rows=512;
    // A profile is at most 1 MiB. A capability reply may repeat it in its
    // applied graph and capability combination, plus a maximal diagnostic.
    l.wire_bytes=l.arena_bytes=l.total_text_bytes=4*1024*1024;l.string_bytes=1024*1024;return l;
}
inline control::numeric::SessionConfig desktop_session_config() {
    control::numeric::SessionConfig config;config.codec_limits=control_limits();
    // 2 wire + 2 aligned arenas + bounded packet/read/identity bookkeeping fit
    // 17 MiB. The poll ledger can count RX plus full TX plus device-pending TX;
    // 13 MiB admits their bounded framed sizes, including packet overhead.
    // PC timers poll at 5 ms. Reading one 4 KiB chunk per callback would impose
    // >2.5 s of avoidable pacing on a 2 MiB State/capability reply. A bounded
    // packet-sized read drains a 4 MiB envelope in about 65 polls (~325 ms at
    // the nominal cadence), leaving the existing 5 s deadline for CPU/I/O.
    // This is within ControlSession's 65535-byte hard read cap; device defaults
    // and wire/arena/peer bounds stay unchanged.
    config.read_chunk_bytes=65535;
    config.storage_budget_bytes=ControlStorageBudgetBytes;config.peer_budget_bytes=ControlPeerBudgetBytes;return config;
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
    Status append_row(const domain::ConfiguredProcessingNode& node,std::size_t index,
                      const application::NumericProcessingCapabilities& profile) {
        domain::ResolvedProcessingNode resolved;
        if(!domain::resolve_processing_node(node,profile.runtime.admission,resolved))return Status::invalid_argument;
        auto support=profile.device.query(profile.device.context,resolved,profile.runtime.resources);
        if(!support.supported)return Status::incompatible_contract;
        const auto r=support.resources;
        rows.push_back({graph.wire().nodes[index],graph.wire().nodes[index].kind,"desktop-cpu",signal_contracts::ValueDomain::FiniteOnly,
            {r.plan_bytes,r.payload_bytes,r.scratch_bytes,r.stack_bytes,r.plan_bytes+r.payload_bytes+r.scratch_bytes+r.stack_bytes}});
        return Status::ok;
    }
    Status assign_rows(const domain::NumericProcessingGraph& g,const application::NumericProcessingCapabilities& profile) {
        if(g.nodes.empty())return Status::ok;
        if(!graph.assign(g,control_limits()))return Status::invalid_argument;
        rows.reserve(g.nodes.size());
        for(std::size_t i=0;i<g.nodes.size();++i) {
            const auto status=append_row(g.nodes[i],i,profile);
            if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    Status assign(const domain::NumericProcessingGraph& g,const application::ProcessingCapabilities& profile) {
        if(!profile.numeric)return Status::invalid_argument;
        const auto& p=*profile.numeric;
        const auto& maximum=p.runtime.resources;
        value.maximum={maximum.maximum.plan_bytes,maximum.maximum.payload_bytes,maximum.maximum.scratch_bytes,
            maximum.maximum.stack_bytes,maximum.max_peak_bytes};
        const auto status=assign_rows(g,p);
        if(status!=Status::ok)return status;
        value.rows=rows;return Status::ok;
    }
};
}
