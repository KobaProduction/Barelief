#include <control/numeric_processing_message_mapper.h>
#include <bdsp_protocol/numeric_control_codec.h>
#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>

namespace control::numeric {
namespace nc=bdsp_protocol::numeric_control;
namespace {
nc::Result failure(nc::Status s) { nc::Result r; r.status=s; return r; }
template<class To> void copy_values(std::span<const nc::Parameter> from, To& to) {
    for(const auto& p:from) {
        domain::ProcessingParameter v; v.key=p.key;
        std::visit([&](const auto& x) {
            using T=std::decay_t<decltype(x)>;
            if constexpr(std::is_same_v<T,std::string_view>) v.value=std::string(x);
            else v.value=x;
        },p.value);
        to.values.push_back(std::move(v));
    }
}
}
nc::Result own_parameters(const nc::Parameters& from,const nc::Limits& limits,domain::ProcessingParameters& out) {
    nc::Message m;nc::Command c;c.id=nc::CommandId::SetParameters;c.node=0;c.parameters=from;m.body=c;
    auto result=nc::validate(m,limits);if(!result)return result;
    domain::ProcessingParameters next;
    std::visit([&](const auto& p) {
        using T=std::decay_t<decltype(p)>;
        if constexpr(std::is_same_v<T,nc::EndpointParameters>) {
            domain::EndpointProcessingParameters v; copy_values(p.values,v); next=std::move(v);
        } else if constexpr(std::is_same_v<T,nc::TransformParameters>) {
            domain::TransformProcessingParameters v; copy_values(p.values,v); next=std::move(v);
        } else if constexpr(std::is_same_v<T,nc::ReductionParameters>) {
            domain::ReduceProcessingParameters v;
            v.operation=p.operation; v.channels=p.channels; v.rows=p.rows;
            v.selected_channels.assign(p.selected_channels.begin(),p.selected_channels.end());
            v.precision_loss=p.precision_loss; v.rounding=p.rounding; next=std::move(v);
        } else if constexpr(std::is_same_v<T,nc::GeneratorParameters>) { domain::SignalGeneratorProcessingParameters g;g.oscillators.assign(p.oscillators.begin(),p.oscillators.end());next=std::move(g);
        } else if constexpr(std::is_same_v<T,nc::ClockParameters>) {next=domain::SampleClockProcessingParameters{p.sample_rate,std::string(p.domain)};
        } else if constexpr(std::is_same_v<T,nc::MixerParameters>) {domain::MixerProcessingParameters m;m.mode=p.mode;m.weights.assign(p.weights.begin(),p.weights.end());m.fifo_milliseconds=p.fifo_milliseconds;m.consumption=p.consumption;next=std::move(m);
        } else next=p;
    },from);
    out=std::move(next); return result;
}
nc::Result own_graph(const nc::Graph& from,const nc::Limits& limits,domain::NumericProcessingGraph& out) {
    nc::Message m; nc::Command c; c.id=nc::CommandId::Prepare; c.graph=from; m.body=c;
    auto result=nc::validate(m,limits); if(!result) return result;
    if(domain::processing_versions_supported(from.versions)!=signal_contracts::Status::ok ||
       domain::processing_boundary_version_supported(domain::ProcessingBoundary::Control,from.versions.control)!=signal_contracts::Status::ok)
        return failure(nc::Status::UnsupportedVersion);
    domain::NumericProcessingGraph next; next.graph_id=from.graph_id; next.versions=from.versions; next.execution_interval=from.execution_interval;
    next.nodes.reserve(from.nodes.size()); next.edges.assign(from.edges.begin(),from.edges.end());next.configuration_bindings.assign(from.configuration_bindings.begin(),from.configuration_bindings.end());
    std::size_t owned=0;
    for(const auto& node:from.nodes) {
        domain::ConfiguredProcessingNode n; n.id=node.id; n.kind=node.kind; n.enabled=node.enabled;
        result=own_parameters(node.parameters,limits,n.parameters);if(!result)return result;
        for(bool input:{true,false}) {
            const auto ports=input?node.inputs:node.outputs; auto& dest=input?n.inputs:n.outputs;
            dest.reserve(ports.size());
            for(const auto& p:ports) {
                domain::ProcessingPort v; v.id=p.id; v.direction=p.direction;
                auto status=domain::own_processing_contract(p.contract,limits.frame,limits.arena_bytes-owned,v.contract);
                if(status!=signal_contracts::Status::ok) return failure(nc::Status::Limit);
                if(v.contract.owned_bytes()>limits.arena_bytes-owned) return failure(nc::Status::Limit);
                owned+=v.contract.owned_bytes(); dest.push_back(std::move(v));
            }
        }
        next.nodes.push_back(std::move(n));
    }
    out=std::move(next); return result;
}
struct GraphProjection::Storage {
    domain::NumericProcessingGraph graph;
    std::vector<nc::Node> nodes;
    std::vector<std::vector<nc::Port>> inputs,outputs;
    std::vector<std::vector<nc::Parameter>> parameters;
    nc::Graph wire;
};
GraphProjection::GraphProjection()=default;
GraphProjection::~GraphProjection()=default;
GraphProjection::GraphProjection(GraphProjection&&) noexcept=default;
GraphProjection& GraphProjection::operator=(GraphProjection&&) noexcept=default;
const nc::Graph& GraphProjection::wire() const noexcept { return storage_->wire; }
nc::Result GraphProjection::assign(const domain::NumericProcessingGraph& from,const nc::Limits& limits) {
    if(domain::processing_versions_supported(from.versions)!=signal_contracts::Status::ok ||
       domain::processing_boundary_version_supported(domain::ProcessingBoundary::Control,from.versions.control)!=signal_contracts::Status::ok)
        return failure(nc::Status::UnsupportedVersion);
    // Bound cold owning copies before allocating them. Complete grammar/contract
    // validation below remains authoritative and publishes atomically.
    if(from.nodes.size()>limits.nodes||from.edges.size()>limits.edges||from.graph_id.size()>limits.string_bytes)
        return failure(nc::Status::Limit);
    std::size_t text=from.graph_id.size(),ports=0,parameters=0,metadata=0;
    auto charge=[](std::size_t& total,std::size_t n,std::size_t max) {
        if(total>max||n>max-total)return false;
        total+=n;return true;
    };
    for(const auto& n:from.nodes) {
        if(n.kind.size()>limits.string_bytes||!charge(text,n.kind.size(),limits.total_text_bytes)||
           !charge(ports,n.inputs.size(),limits.ports)||!charge(ports,n.outputs.size(),limits.ports))return failure(nc::Status::Limit);
        for(const auto& p:n.inputs)if(!p.contract||!charge(metadata,p.contract.owned_bytes(),limits.arena_bytes))return failure(nc::Status::Limit);
        for(const auto& p:n.outputs)if(!p.contract||!charge(metadata,p.contract.owned_bytes(),limits.arena_bytes))return failure(nc::Status::Limit);
        bool bounded=std::visit([&](const auto& p) {
            using T=std::decay_t<decltype(p)>;
            if constexpr(std::is_same_v<T,domain::EndpointProcessingParameters>||std::is_same_v<T,domain::TransformProcessingParameters>) {
                if(!charge(parameters,p.values.size(),limits.parameters))return false;
                for(const auto& v:p.values) {
                    if(v.key.size()>limits.string_bytes||!charge(text,v.key.size(),limits.total_text_bytes))return false;
                    if(const auto* s=std::get_if<std::string>(&v.value))
                        if(s->size()>limits.string_bytes||!charge(text,s->size(),limits.total_text_bytes))return false;
                }
            } else if constexpr(std::is_same_v<T,domain::ReduceProcessingParameters>) return p.selected_channels.size()<=limits.channels;
            else if constexpr(std::is_same_v<T,domain::SignalGeneratorProcessingParameters>) return p.oscillators.size()<=128 && charge(parameters,p.oscillators.size(),limits.parameters);
            else if constexpr(std::is_same_v<T,domain::SampleClockProcessingParameters>) return p.domain.size()<=limits.string_bytes && charge(text,p.domain.size(),limits.total_text_bytes);
            else if constexpr(std::is_same_v<T,domain::MixerProcessingParameters>) return p.weights.size()<=limits.ports && charge(parameters,p.weights.size(),limits.parameters);
            return true;
        },n.parameters);
        if(!bounded)return failure(nc::Status::Limit);
    }
    auto next=std::make_unique<Storage>(); next->graph=from;
    const auto count=from.nodes.size(); next->nodes.reserve(count);
    next->inputs.resize(count); next->outputs.resize(count); next->parameters.resize(count);
    for(std::size_t i=0;i<count;++i) {
        const auto& n=next->graph.nodes[i]; nc::Node w; w.id=n.id; w.kind=n.kind; w.enabled=n.enabled;
        for(bool input:{true,false}) {
            auto& dest=input?next->inputs[i]:next->outputs[i];
            for(const auto& p:input?n.inputs:n.outputs) dest.push_back({p.id,p.direction,p.contract.view()});
        }
        w.inputs=next->inputs[i]; w.outputs=next->outputs[i];
        std::visit([&](const auto& p) {
            using T=std::decay_t<decltype(p)>;
            if constexpr(std::is_same_v<T,domain::EndpointProcessingParameters> || std::is_same_v<T,domain::TransformProcessingParameters>) {
                auto& values=next->parameters[i];
                for(const auto& v:p.values) {
                    nc::Parameter q; q.key=v.key;
                    std::visit([&](const auto& x) {
                        using V=std::decay_t<decltype(x)>;
                        if constexpr(std::is_same_v<V,std::string>) q.value=std::string_view(x); else q.value=x;
                    },v.value); values.push_back(q);
                }
                if constexpr(std::is_same_v<T,domain::EndpointProcessingParameters>) w.parameters=nc::EndpointParameters{values};
                else w.parameters=nc::TransformParameters{values};
            } else if constexpr(std::is_same_v<T,domain::ReduceProcessingParameters>) {
                w.parameters=nc::ReductionParameters{p.operation,p.channels,p.rows,p.selected_channels,p.precision_loss,p.rounding};
            } else if constexpr(std::is_same_v<T,domain::SignalGeneratorProcessingParameters>) w.parameters=nc::GeneratorParameters{p.oscillators};
            else if constexpr(std::is_same_v<T,domain::SampleClockProcessingParameters>) w.parameters=nc::ClockParameters{p.sample_rate,p.domain};
            else if constexpr(std::is_same_v<T,domain::MixerProcessingParameters>) w.parameters=nc::MixerParameters{p.mode,p.weights,p.fifo_milliseconds,p.consumption};
            else w.parameters=p;
        },n.parameters);
        next->nodes.push_back(w);
    }
    next->wire={next->graph.graph_id,next->graph.versions,next->nodes,next->graph.edges,next->graph.execution_interval,next->graph.configuration_bindings};
    // Validate before publishing, with the exact same boundary grammar as ingress.
    nc::Message m; nc::Command c; c.id=nc::CommandId::Prepare; c.graph=next->wire; m.body=c;
    auto result=nc::validate(m,limits); if(!result) return result;
    storage_=std::move(next); return result;
}
nc::Result reply_envelope_fits(const domain::NumericProcessingGraph& graph,const nc::Limits& limits,
                             const nc::Capabilities& capabilities) {
    GraphProjection projection;
    auto result=projection.assign(graph,limits);if(!result)return result;
    constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
    nc::Reply reply;reply.command=nc::CommandId::Capabilities;reply.error=nc::Error::Validation;
    reply.diagnostic.node=std::numeric_limits<std::int32_t>::min();
    reply.diagnostic.port=std::numeric_limits<std::uint16_t>::max();reply.diagnostic.edge=maximum;
    std::string parameter(limits.string_bytes,'x');reply.diagnostic.parameter=parameter;
    reply.state.lifecycle=nc::Lifecycle::Running;reply.state.run_intent=true;
    reply.state.revision=reply.state.token=maximum;reply.state.applied=projection.wire();
    reply.state.resources={maximum,maximum,maximum,maximum,maximum};reply.capabilities=capabilities;
    nc::Message message;message.client=message.session=message.epoch=message.request=message.expected_revision=maximum;
    message.body=reply;
    std::array<std::uint8_t,16> revisions{};nc::Extension extension{1024,revisions};
    message.extensions=std::span(&extension,1);
    nc::CborCodec cbor;nc::ProtobufCodec protobuf;nc::Result maximum_result;
    // All command/error/lifecycle enum symbols use the same maximum single-byte
    // value class in both codecs. Removing optional fields can only shrink this
    // complete envelope; candidate contracts/parameters remain exact.
    for(const nc::Codec* codec:{static_cast<const nc::Codec*>(&cbor),static_cast<const nc::Codec*>(&protobuf)}) {
        result=codec->measure(message,limits);if(!result)return result;
        maximum_result.required_wire=std::max(maximum_result.required_wire,result.required_wire);
        maximum_result.required_scratch=std::max(maximum_result.required_scratch,result.required_scratch);
        maximum_result.required_arena=std::max(maximum_result.required_arena,result.required_arena);
        maximum_result.required_text=std::max(maximum_result.required_text,result.required_text);
    }
    return maximum_result;
}
}
