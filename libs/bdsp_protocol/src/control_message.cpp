#include <bdsp_protocol/control_codec.h>
#include <cstring>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

namespace bdsp_protocol {
namespace {
bool utf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i++]);
        if (c == 0) return false;
        if (c < 0x80) continue;
        unsigned remaining = 0; std::uint32_t code = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { remaining=1; code=c&31; minimum=0x80; }
        else if (c >= 0xe0 && c <= 0xef) { remaining=2; code=c&15; minimum=0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { remaining=3; code=c&7; minimum=0x10000; }
        else return false;
        if (remaining > text.size()-i) return false;
        while (remaining--) {
            c=static_cast<unsigned char>(text[i++]);
            if ((c&0xc0)!=0x80) return false;
            code=(code<<6)|(c&63);
        }
        if (code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff)) return false;
    }
    return true;
}
struct Validator {
    const ControlCodecLimits& limits;
    ControlStorageRequirements required;
    ControlCodecStatus error = ControlCodecStatus::ok;
    bool reject(ControlCodecStatus status = ControlCodecStatus::malformed) noexcept {
        if (error==ControlCodecStatus::ok) error=status;
        return false;
    }
    bool count(std::size_t& total, std::size_t value, std::size_t maximum) noexcept {
        if (value>std::numeric_limits<std::size_t>::max()-total) return reject(ControlCodecStatus::overflow);
        if (value>maximum || total>maximum-value) return reject(ControlCodecStatus::limit_exceeded);
        total+=value; return true;
    }
    bool text(std::string_view value, std::size_t maximum) noexcept {
        if (value.size()>maximum) return reject(ControlCodecStatus::limit_exceeded);
        if (!utf8(value)) return reject();
        return count(required.text_bytes,value.size(),limits.max_total_string_bytes);
    }
    bool frame(WireFrameType value) noexcept {
        return unsigned(value)<=8 || reject();
    }
    bool parameters(const WireParameters& value) noexcept {
        return std::visit([&](const auto& p) {
            using T=std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T,WireGain>) return std::isfinite(p.gain) || reject();
            else if constexpr (std::is_same_v<T,WireTestSignal>)
                return (std::isfinite(p.minimum_hz) && std::isfinite(p.maximum_hz) &&
                    std::isfinite(p.amplitude) && std::isfinite(p.period_seconds) && std::isfinite(p.sweep_depth)) || reject();
            else if constexpr (std::is_same_v<T,WireNetwork>)
                return frame(p.data_type) && text(p.provider_uuid,limits.max_field_string_bytes) &&
                    text(p.stream_id,limits.max_field_string_bytes) && text(p.host,limits.max_field_string_bytes);
            else if constexpr (std::is_same_v<T,WireRouting> || std::is_same_v<T,WireReduction>) return frame(p.data_type);
            else return true;
        },value);
    }
    bool kind_parameters(const WireNode& node) noexcept {
        // Canonical IDs and processing alternatives from accepted domain descriptors.
        // Range/business checks (gain, geometry, supported kinds) remain in domain.
        constexpr std::pair<std::string_view,std::size_t> kinds[]{
            {"AudioSource",1},{"StereoFFT",0},{"SpectrumMerge",2},{"SpectrumGain",3},
            {"Mel",4},{"Spectrogram",5},{"FFTView",0},{"MelView",0},{"SpectrogramView",0},
            {"AudioView",0},{"WLED",3},{"TestSignal",8},{"Window",6},{"WindowFunction",7},
            {"BarkWeighting",0},{"WindowView",0},{"WindowedView",0},{"NetworkInput",9},
            {"NetworkOutput",9},{"Splitter",10},{"Switch",10},{"Gate",10},{"ScalarReduce",11},
            {"ScalarView",0},{"WLEDEncode",3},{"WLEDOutput",0}
        };
        for (auto [kind,index] : kinds)
            if (kind==node.kind) return node.parameters.index()==index || reject();
        // A future/unsupported canonical kind is an application admission error;
        // typed parameter payload still has no arbitrary/UI field escape.
        return true;
    }
    bool graph(const WireProcessingGraph& value) noexcept {
        if (!text(value.graph_identity,limits.max_graph_identity_bytes) ||
            !count(required.nodes,value.nodes.size(),limits.max_nodes) ||
            !count(required.edges,value.edges.size(),limits.max_edges)) return false;
        for (const auto& node : value.nodes)
            if (!text(node.kind,limits.max_field_string_bytes) || !kind_parameters(node) || !parameters(node.parameters)) return false;
        return true;
    }
    bool command(const WireCommand& value) noexcept {
        const auto id=unsigned(value.id);
        if (id<1 || id>9) return reject();
        const bool prepare=value.id==WireCommandId::prepare;
        const bool apply=value.id==WireCommandId::apply;
        const bool release=value.id==WireCommandId::release;
        const bool set=value.id==WireCommandId::set_parameters;
        const bool persist=value.id==WireCommandId::persist;
        if (bool(value.processing)!=prepare || bool(value.parameters)!=set ||
            (!apply && !value.graph_identity.empty()) || (!apply && !release && value.token!=0) ||
            (!set && (value.node!=-1 || !value.enabled)) || (!persist && value.graph_revision!=0)) return reject();
        if (prepare && !graph(*value.processing)) return false;
        if (apply && !text(value.graph_identity,limits.max_graph_identity_bytes)) return false;
        if (set && !parameters(*value.parameters)) return false;
        return true;
    }
    bool diagnostic(const WireDiagnostic& value) noexcept {
        if (value.direction && unsigned(*value.direction)>1) return reject();
        return text(value.parameter_key,limits.max_field_string_bytes);
    }
    bool capabilities(const WireCapabilities& value) noexcept {
        if (!count(required.capability_kinds,value.node_kinds.size(),limits.max_capability_kinds) ||
            !count(required.receive_types,value.receive_types.size(),limits.max_capability_types) ||
            !count(required.publish_types,value.publish_types.size(),limits.max_capability_types)) return false;
        for (auto kind : value.node_kinds) if (!text(kind,limits.max_field_string_bytes)) return false;
        for (auto type : value.receive_types) if (!frame(type)) return false;
        for (auto type : value.publish_types) if (!frame(type)) return false;
        return true;
    }
    bool reply(const WireReply& value) noexcept {
        if (unsigned(value.error)>12 || unsigned(value.status)>10 || unsigned(value.validation_error)>13 ||
            unsigned(value.state.lifecycle)>3) return reject();
        return diagnostic(value.validation_location) && diagnostic(value.error_location) &&
            (!value.state.applied || graph(*value.state.applied)) &&
            (!value.capabilities || capabilities(*value.capabilities));
    }
};
}
ControlCodecResult validate_control_message(const WireControlMessage& value, const ControlCodecLimits& limits) noexcept {
    ControlCodecResult result;
    if (value.schema_version!=control_schema_version) {
        result.status=ControlCodecStatus::unsupported_version; return result;
    }
    Validator validator{limits,{}};
    bool valid=std::visit([&](const auto& body) {
        using T=std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T,WireCommand>) return validator.command(body);
        else return validator.reply(body);
    },value.body);
    result.status=valid?ControlCodecStatus::ok:validator.error;
    if (valid) result.required_arena=validator.required;
    return result;
}
std::optional<ControlCodecId> select_control_codec(std::uint32_t local, std::uint32_t remote,
    std::uint32_t local_version, std::uint32_t remote_version) noexcept {
    if (local_version!=control_schema_version || remote_version!=control_schema_version) return {};
    if ((local&remote&control_cbor_mask)!=0) return ControlCodecId::cbor;
    if ((local&remote&control_protobuf_mask)!=0) return ControlCodecId::protobuf;
    return {};
}

namespace detail {
unsigned control_schema_max_tag(ControlSchema schema) noexcept {
    constexpr unsigned counts[]{3,0,1,1,1,1,1,1,1,7,5,3,2,12,4,4,4,10,5,3,8,20,14,4,0,0};
    return counts[static_cast<unsigned>(schema)];
}
bool ControlArchiveState::fail(ControlCodecStatus status) noexcept {
    if (error==ControlCodecStatus::ok) error=status;
    return false;
}
bool ControlArchiveState::count(std::size_t& value, std::size_t maximum) noexcept {
    if (value>=maximum) return fail(ControlCodecStatus::limit_exceeded);
    ++value; return true;
}
bool ControlArchiveState::store_text(std::string_view& value, std::size_t maximum) noexcept {
    if (!reading) return true;
    if (value.size()>maximum || value.size()>limits.max_total_string_bytes ||
        used.text_bytes>limits.max_total_string_bytes-value.size()) return fail(ControlCodecStatus::limit_exceeded);
    if (!utf8(value)) return fail();
    if (materializing && !value.empty()) {
        char* output=arena.text.data()+used.text_bytes;
        std::memcpy(output,value.data(),value.size());
        value={output,value.size()};
    } else if (materializing) value={};
    used.text_bytes+=value.size(); return true;
}
bool control_arena_fits(const ControlStorageRequirements& r, ControlDecodeArena a) noexcept {
    return r.nodes<=a.nodes.size() && r.edges<=a.edges.size() && r.text_bytes<=a.text.size() &&
        r.capability_kinds<=a.capability_kinds.size() && r.receive_types<=a.receive_types.size() &&
        r.publish_types<=a.publish_types.size();
}
bool validate_control_node(const WireNode& node, const ControlCodecLimits& limits) noexcept {
    Validator validator{limits,{}};
    return validator.kind_parameters(node) && validator.parameters(node.parameters);
}
namespace {
template<class T> bool unsigned_field(ControlArchive& a, unsigned tag, T& value) noexcept {
    std::uint64_t integer=static_cast<std::uint64_t>(value);
    if (!a.uint_field(tag,integer)) return false;
    if constexpr (std::is_enum_v<T>) {
        if (integer>std::numeric_limits<std::underlying_type_t<T>>::max()) return a.state.fail();
    } else if (integer>std::numeric_limits<T>::max()) return a.state.fail();
    if (a.state.reading) value=static_cast<T>(integer);
    return true;
}
template<class T> bool optional_object(ControlArchive& a, unsigned tag, ControlSchema schema,
    std::optional<T>& value, ControlVisit visit) noexcept {
    bool present=value.has_value();
    T item=value.value_or(T{});
    if (!a.object_field(tag,schema,&item,visit,&present)) return false;
    if (a.state.reading) { if (present) value=item; else value.reset(); }
    return true;
}
template<class T> bool optional_unsigned(ControlArchive& a, unsigned tag, std::optional<T>& value) noexcept {
    bool present=value.has_value();
    std::uint64_t integer=present?static_cast<std::uint64_t>(*value):0;
    if (!a.optional_uint_field(tag,integer,present)) return false;
    if (present) {
        if constexpr (std::is_enum_v<T>) {
            if (integer>std::numeric_limits<std::underlying_type_t<T>>::max()) return a.state.fail();
        } else if (integer>std::numeric_limits<T>::max()) return a.state.fail();
        if (a.state.reading) value=static_cast<T>(integer);
    } else if (a.state.reading) value.reset();
    return true;
}
bool visit_SessionIdentity(ControlArchive&,void*) noexcept;
bool visit_NoParameters(ControlArchive&,void*) noexcept;
bool visit_AudioSource(ControlArchive&,void*) noexcept;
bool visit_Merge(ControlArchive&,void*) noexcept;
bool visit_Gain(ControlArchive&,void*) noexcept;
bool visit_Mel(ControlArchive&,void*) noexcept;
bool visit_History(ControlArchive&,void*) noexcept;
bool visit_Window(ControlArchive&,void*) noexcept;
bool visit_WindowFunction(ControlArchive&,void*) noexcept;
bool visit_TestSignal(ControlArchive&,void*) noexcept;
bool visit_Network(ControlArchive&,void*) noexcept;
bool visit_Routing(ControlArchive&,void*) noexcept;
bool visit_Reduction(ControlArchive&,void*) noexcept;
bool visit_Edge(ControlArchive&,void*) noexcept;
bool visit_Memory(ControlArchive&,void*) noexcept;
bool visit_Parameters(ControlArchive&,void*) noexcept;
bool visit_Node(ControlArchive&,void*) noexcept;
bool visit_ProcessingGraph(ControlArchive&,void*) noexcept;
bool visit_Command(ControlArchive&,void*) noexcept;
bool visit_Diagnostic(ControlArchive&,void*) noexcept;
bool visit_State(ControlArchive&,void*) noexcept;
bool visit_Capabilities(ControlArchive&,void*) noexcept;
bool visit_Reply(ControlArchive&,void*) noexcept;
bool visit_SessionIdentity(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireSessionIdentity*>(pointer);
    return unsigned_field(a,1,v.client) &&
        unsigned_field(a,2,v.session) &&
        unsigned_field(a,3,v.epoch);
}
bool visit_NoParameters(ControlArchive& a, void* pointer) noexcept {
    (void)a; (void)pointer; return true;
}
bool visit_AudioSource(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireAudioSource*>(pointer);
    return unsigned_field(a,1,v.source);
}
bool visit_Merge(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireMerge*>(pointer);
    return unsigned_field(a,1,v.merge);
}
bool visit_Gain(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireGain*>(pointer);
    return a.double_field(1,v.gain);
}
bool visit_Mel(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireMel*>(pointer);
    return unsigned_field(a,1,v.bands);
}
bool visit_History(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireHistory*>(pointer);
    return unsigned_field(a,1,v.history_size);
}
bool visit_Window(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireWindow*>(pointer);
    return unsigned_field(a,1,v.size);
}
bool visit_WindowFunction(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireWindowFunction*>(pointer);
    return unsigned_field(a,1,v.function);
}
bool visit_TestSignal(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireTestSignal*>(pointer);
    return unsigned_field(a,1,v.tones) &&
        unsigned_field(a,2,v.sample_rate) &&
        a.double_field(3,v.minimum_hz) &&
        a.double_field(4,v.maximum_hz) &&
        a.double_field(5,v.amplitude) &&
        a.double_field(6,v.period_seconds) &&
        a.double_field(7,v.sweep_depth);
}
bool visit_Network(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireNetwork*>(pointer);
    return unsigned_field(a,1,v.data_type) &&
        a.text_field(2,v.provider_uuid,a.state.limits.max_field_string_bytes) &&
        a.text_field(3,v.stream_id,a.state.limits.max_field_string_bytes) &&
        a.text_field(4,v.host,a.state.limits.max_field_string_bytes) &&
        unsigned_field(a,5,v.port);
}
bool visit_Routing(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireRouting*>(pointer);
    return unsigned_field(a,1,v.data_type) &&
        unsigned_field(a,2,v.outputs) &&
        unsigned_field(a,3,v.selected);
}
bool visit_Reduction(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireReduction*>(pointer);
    return unsigned_field(a,1,v.data_type) &&
        unsigned_field(a,2,v.operation);
}
bool visit_Edge(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireEdge*>(pointer);
    return a.int_field(1,v.from) &&
        a.int_field(2,v.to) &&
        unsigned_field(a,3,v.output_port) &&
        unsigned_field(a,4,v.input_port);
}
bool visit_Memory(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireMemory*>(pointer);
    return unsigned_field(a,1,v.state_bytes) &&
        unsigned_field(a,2,v.buffer_bytes) &&
        unsigned_field(a,3,v.scratch_bytes);
}

// Optional scalar presence is separate from its numeric value: zero is data.

bool visit_Parameters(ControlArchive& a, void* pointer) noexcept {
    auto& parameters=*static_cast<WireParameters*>(pointer);
    unsigned selected=0;
    {
        bool present=!a.state.reading && parameters.index()==0;
        WireNoParameters item{};
        if (present) item=std::get<WireNoParameters>(parameters);
        if (!a.object_field(1,ControlSchema::NoParameters,&item,visit_NoParameters,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    {
        bool present=!a.state.reading && parameters.index()==1;
        WireAudioSource item{};
        if (present) item=std::get<WireAudioSource>(parameters);
        if (!a.object_field(2,ControlSchema::AudioSource,&item,visit_AudioSource,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    {
        bool present=!a.state.reading && parameters.index()==2;
        WireMerge item{};
        if (present) item=std::get<WireMerge>(parameters);
        if (!a.object_field(3,ControlSchema::Merge,&item,visit_Merge,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    {
        bool present=!a.state.reading && parameters.index()==3;
        WireGain item{};
        if (present) item=std::get<WireGain>(parameters);
        if (!a.object_field(4,ControlSchema::Gain,&item,visit_Gain,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    {
        bool present=!a.state.reading && parameters.index()==4;
        WireMel item{};
        if (present) item=std::get<WireMel>(parameters);
        if (!a.object_field(5,ControlSchema::Mel,&item,visit_Mel,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    {
        bool present=!a.state.reading && parameters.index()==5;
        WireHistory item{};
        if (present) item=std::get<WireHistory>(parameters);
        if (!a.object_field(6,ControlSchema::History,&item,visit_History,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    {
        bool present=!a.state.reading && parameters.index()==6;
        WireWindow item{};
        if (present) item=std::get<WireWindow>(parameters);
        if (!a.object_field(7,ControlSchema::Window,&item,visit_Window,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    {
        bool present=!a.state.reading && parameters.index()==7;
        WireWindowFunction item{};
        if (present) item=std::get<WireWindowFunction>(parameters);
        if (!a.object_field(8,ControlSchema::WindowFunction,&item,visit_WindowFunction,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    {
        bool present=!a.state.reading && parameters.index()==8;
        WireTestSignal item{};
        if (present) item=std::get<WireTestSignal>(parameters);
        if (!a.object_field(9,ControlSchema::TestSignal,&item,visit_TestSignal,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    {
        bool present=!a.state.reading && parameters.index()==9;
        WireNetwork item{};
        if (present) item=std::get<WireNetwork>(parameters);
        if (!a.object_field(10,ControlSchema::Network,&item,visit_Network,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    {
        bool present=!a.state.reading && parameters.index()==10;
        WireRouting item{};
        if (present) item=std::get<WireRouting>(parameters);
        if (!a.object_field(11,ControlSchema::Routing,&item,visit_Routing,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    {
        bool present=!a.state.reading && parameters.index()==11;
        WireReduction item{};
        if (present) item=std::get<WireReduction>(parameters);
        if (!a.object_field(12,ControlSchema::Reduction,&item,visit_Reduction,&present)) return false;
        if (present) { ++selected; if (a.state.reading) parameters=item; }
    }
    return selected==1 || a.state.fail();
}
bool visit_Node(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireNode*>(pointer);
    if (!a.int_field(1,v.id) || !a.text_field(2,v.kind,a.state.limits.max_field_string_bytes) ||
        !a.object_field(3,ControlSchema::Parameters,&v.parameters,visit_Parameters) ||
        !a.bool_field(4,v.enabled)) return false;
    return validate_control_node(v,a.state.limits) || a.state.fail();
}
bool node_element(ControlArchive& a, const void* data, std::size_t index) noexcept {
    auto& s=a.state;
    WireNode value=s.reading?WireNode{}:static_cast<const WireNode*>(data)[index];
    const auto slot=s.used.nodes;
    if (s.reading && !s.count(s.used.nodes,s.limits.max_nodes)) return false;
    if (!visit_Node(a,&value)) return false;
    if (s.materializing) s.arena.nodes[slot]=value;
    return true;
}
bool edge_element(ControlArchive& a, const void* data, std::size_t index) noexcept {
    auto& s=a.state;
    WireEdge value=s.reading?WireEdge{}:static_cast<const WireEdge*>(data)[index];
    const auto slot=s.used.edges;
    if (s.reading && !s.count(s.used.edges,s.limits.max_edges)) return false;
    if (!visit_Edge(a,&value)) return false;
    if (s.materializing) s.arena.edges[slot]=value;
    return true;
}
bool visit_ProcessingGraph(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireProcessingGraph*>(pointer);
    auto node_start=a.state.used.nodes, edge_start=a.state.used.edges;
    std::size_t nodes=v.nodes.size(), edges=v.edges.size();
    if (!a.text_field(1,v.graph_identity,a.state.limits.max_graph_identity_bytes) ||
        !unsigned_field(a,2,v.processing_version) ||
        !a.sequence_field(3,ControlSchema::Node,v.nodes.data(),nodes,node_element) ||
        !a.sequence_field(4,ControlSchema::Edge,v.edges.data(),edges,edge_element)) return false;
    if (a.state.reading) {
        v.nodes=a.state.materializing?a.state.arena.nodes.subspan(node_start,nodes):std::span<const WireNode>{};
        v.edges=a.state.materializing?a.state.arena.edges.subspan(edge_start,edges):std::span<const WireEdge>{};
    }
    return true;
}
bool visit_Command(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireCommand*>(pointer);
    if (!unsigned_field(a,1,v.id) || !unsigned_field(a,2,v.request_id) || !unsigned_field(a,3,v.expected_revision)) return false;
    switch (v.id) {
    case WireCommandId::capabilities: case WireCommandId::state: case WireCommandId::start: case WireCommandId::stop: return true;
    case WireCommandId::prepare:
        if (a.state.reading) v.processing.emplace();
        return a.object_field(4,ControlSchema::ProcessingGraph,&*v.processing,visit_ProcessingGraph);
    case WireCommandId::apply:
        return unsigned_field(a,5,v.token) && a.text_field(6,v.graph_identity,a.state.limits.max_graph_identity_bytes);
    case WireCommandId::release: return unsigned_field(a,5,v.token);
    case WireCommandId::set_parameters:
        if (a.state.reading) v.parameters.emplace();
        return a.int_field(7,v.node) && a.object_field(8,ControlSchema::Parameters,&*v.parameters,visit_Parameters) &&
            a.bool_field(9,v.enabled);
    case WireCommandId::persist: return unsigned_field(a,10,v.graph_revision);
    default: return a.state.fail();
    }
}
bool visit_Diagnostic(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireDiagnostic*>(pointer);
    return a.int_field(1,v.node) && a.text_field(2,v.parameter_key,a.state.limits.max_field_string_bytes) &&
        optional_unsigned(a,3,v.direction) && optional_unsigned(a,4,v.port) && optional_unsigned(a,5,v.edge_index);
}
bool visit_State(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireState*>(pointer);
    return unsigned_field(a,1,v.epoch) && unsigned_field(a,2,v.revision) && unsigned_field(a,3,v.graph_revision) &&
        unsigned_field(a,4,v.persisted_graph_revision) && unsigned_field(a,5,v.lifecycle) && a.bool_field(6,v.run_requested) &&
        optional_object(a,7,ControlSchema::ProcessingGraph,v.applied,visit_ProcessingGraph) && unsigned_field(a,8,v.own_prepared_token);
}
bool kind_element(ControlArchive& a, const void* data, std::size_t index) noexcept {
    auto& s=a.state;
    std::string_view value=s.reading?std::string_view{}:static_cast<const std::string_view*>(data)[index];
    const auto slot=s.used.capability_kinds;
    if (s.reading && !s.count(s.used.capability_kinds,s.limits.max_capability_kinds)) return false;
    if (!a.text_field(0,value,s.limits.max_field_string_bytes)) return false;
    if (s.materializing) s.arena.capability_kinds[slot]=value;
    return true;
}
bool type_element(ControlArchive& a, const void* data, std::size_t index) noexcept {
    WireFrameType value=a.state.reading?WireFrameType::pcm:static_cast<const WireFrameType*>(data)[index];
    if (!unsigned_field(a,0,value) || unsigned(value)>8) return a.state.fail();
    // sequence caller allocates typed span before entering; index is local.
    if (a.state.materializing) static_cast<WireFrameType*>(const_cast<void*>(data))[index]=value;
    return true;
}
bool visit_Capabilities(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireCapabilities*>(pointer);
    auto kind_start=a.state.used.capability_kinds;
    std::size_t kinds=v.node_kinds.size(), receive=v.receive_types.size(), publish=v.publish_types.size();
    if (!a.sequence_field(1,ControlSchema::TextElement,v.node_kinds.data(),kinds,kind_element)) return false;
    const void* receive_data=a.state.reading?a.state.arena.receive_types.data():v.receive_types.data();
    const void* publish_data=a.state.reading?a.state.arena.publish_types.data():v.publish_types.data();
    if (!a.sequence_field(2,ControlSchema::TypeElement,receive_data,receive,type_element) ||
        !a.sequence_field(3,ControlSchema::TypeElement,publish_data,publish,type_element)) return false;
    if (a.state.reading) {
        a.state.used.receive_types=receive; a.state.used.publish_types=publish;
        v.node_kinds=a.state.materializing?a.state.arena.capability_kinds.subspan(kind_start,kinds):std::span<const std::string_view>{};
        v.receive_types=a.state.materializing?a.state.arena.receive_types.first(receive):std::span<const WireFrameType>{};
        v.publish_types=a.state.materializing?a.state.arena.publish_types.first(publish):std::span<const WireFrameType>{};
    }
    return unsigned_field(a,4,v.processing_version) &&
        unsigned_field(a,5,v.max_wled_outputs) &&
        unsigned_field(a,6,v.max_nodes) &&
        unsigned_field(a,7,v.max_edges) &&
        unsigned_field(a,8,v.max_memory_bytes) &&
        unsigned_field(a,9,v.max_frame_samples) &&
        unsigned_field(a,10,v.max_history_frames) &&
        unsigned_field(a,11,v.max_sessions) &&
        unsigned_field(a,12,v.max_pending_prepared) &&
        unsigned_field(a,13,v.prepared_ttl_ms) &&
        unsigned_field(a,14,v.max_graph_identity_bytes) &&
        unsigned_field(a,15,v.max_prepared_bytes) &&
        unsigned_field(a,16,v.max_field_string_bytes) &&
        unsigned_field(a,17,v.max_replay_bytes_per_session) &&
        unsigned_field(a,18,v.max_replay_bytes_total) &&
        unsigned_field(a,19,v.max_control_scratch_bytes) && a.bool_field(20,v.explicit_persist);
}
bool visit_Reply(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireReply*>(pointer);
    return unsigned_field(a,1,v.request_id) && a.bool_field(2,v.accepted) && unsigned_field(a,3,v.error) &&
        unsigned_field(a,4,v.status) && unsigned_field(a,5,v.validation_error) && a.int_field(6,v.validation_node) &&
        unsigned_field(a,7,v.validation_edge_index) &&
        a.object_field(8,ControlSchema::Diagnostic,&v.validation_location,visit_Diagnostic) &&
        a.object_field(9,ControlSchema::Diagnostic,&v.error_location,visit_Diagnostic) &&
        a.object_field(10,ControlSchema::State,&v.state,visit_State) && unsigned_field(a,11,v.prepared_token) &&
        unsigned_field(a,12,v.expires_at_ms) && a.object_field(13,ControlSchema::Memory,&v.memory,visit_Memory) &&
        optional_object(a,14,ControlSchema::Capabilities,v.capabilities,visit_Capabilities);
}
}
bool visit_control_message(ControlArchive& a, void* pointer) noexcept {
    auto& v=*static_cast<WireControlMessage*>(pointer);
    if (!unsigned_field(a,1,v.schema_version)) return false;
    if (v.schema_version!=control_schema_version) return a.state.fail(ControlCodecStatus::unsupported_version);
    if (!a.object_field(2,ControlSchema::SessionIdentity,&v.identity,visit_SessionIdentity)) return false;
    bool command_present=!a.state.reading && std::holds_alternative<WireCommand>(v.body);
    bool reply_present=!a.state.reading && !command_present;
    WireCommand command=command_present?std::get<WireCommand>(v.body):WireCommand{};
    WireReply reply=reply_present?std::get<WireReply>(v.body):WireReply{};
    if (!a.object_field(3,ControlSchema::Command,&command,visit_Command,&command_present) ||
        !a.object_field(4,ControlSchema::Reply,&reply,visit_Reply,&reply_present)) return false;
    if (command_present==reply_present) return a.state.fail();
    if (a.state.reading) { if (command_present) v.body=command; else v.body=reply; }
    const auto result=validate_control_message(v,a.state.limits);
    return result || a.state.fail(result.status);
}
} // namespace detail
} // namespace bdsp_protocol
