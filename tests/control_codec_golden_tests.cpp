#include <bdsp_protocol/control_message.h>
#include <bdsp_protocol/control_codec.h>
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <cstdlib>
#include <new>
#include <application/processing_control.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace bdsp_protocol;
// Golden semantic IDs are explicit; no mapper may cast variant indices to wire IDs.
static_assert(unsigned(WireControlError::persistence_failure) == 12);
static_assert(unsigned(WireGraphError::unsupported_version) == 13);
static_assert(unsigned(WireCommandId::persist) == 9);
static_assert(unsigned(WireStatus::unsupported_version) == 10);
static_assert(unsigned(WireFrameType::wled_packet) == 8);
static_assert(std::variant_size_v<application::ProcessingCommand> == 9);
static_assert(std::variant_size_v<WireParameters> == 12); // no FFTView alternative

static bool forbid_allocations = false;
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __real_calloc(std::size_t,std::size_t);
extern "C" void* __real_realloc(void*,std::size_t);
extern "C" void* __wrap_malloc(std::size_t size) {
    if(forbid_allocations)std::abort();
    return __real_malloc(size);
}
extern "C" void* __wrap_calloc(std::size_t count,std::size_t size) {
    if(forbid_allocations)std::abort();
    return __real_calloc(count,size);
}
extern "C" void* __wrap_realloc(void* pointer,std::size_t size) {
    if(forbid_allocations)std::abort();
    return __real_realloc(pointer,size);
}
void* operator new(std::size_t size) {
    if(forbid_allocations)std::abort();
    if(auto* pointer=std::malloc(size?size:1))return pointer;
    std::abort();
}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* pointer) noexcept {std::free(pointer);}
void operator delete[](void* pointer) noexcept {std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept {std::free(pointer);}
void operator delete[](void* pointer,std::size_t) noexcept {std::free(pointer);}

namespace {
bool same_double(double a,double b) {
    std::uint64_t aa=0,bb=0;
    std::memcpy(&aa,&a,sizeof(a)); std::memcpy(&bb,&b,sizeof(b));
    return aa==bb;
}
bool same_parameters(const WireParameters& a,const WireParameters& b) {
    if(a.index()!=b.index())return false;
    return std::visit([&](const auto& left) {
        using T=std::decay_t<decltype(left)>;
        const auto& right=std::get<T>(b);
        if constexpr(std::is_same_v<T,WireNoParameters>) { (void)left; (void)right; return true; }
        else if constexpr(std::is_same_v<T,WireAudioSource>) { return left.source==right.source; }
        else if constexpr(std::is_same_v<T,WireMerge>) { return left.merge==right.merge; }
        else if constexpr(std::is_same_v<T,WireGain>) { return same_double(left.gain,right.gain); }
        else if constexpr(std::is_same_v<T,WireMel>) { return left.bands==right.bands; }
        else if constexpr(std::is_same_v<T,WireHistory>) { return left.history_size==right.history_size; }
        else if constexpr(std::is_same_v<T,WireWindow>) { return left.size==right.size; }
        else if constexpr(std::is_same_v<T,WireWindowFunction>) { return left.function==right.function; }
        else if constexpr(std::is_same_v<T,WireTestSignal>) { return left.tones==right.tones && left.sample_rate==right.sample_rate && same_double(left.minimum_hz,right.minimum_hz) && same_double(left.maximum_hz,right.maximum_hz) && same_double(left.amplitude,right.amplitude) && same_double(left.period_seconds,right.period_seconds) && same_double(left.sweep_depth,right.sweep_depth); }
        else if constexpr(std::is_same_v<T,WireNetwork>) { return left.data_type==right.data_type && left.provider_uuid==right.provider_uuid && left.stream_id==right.stream_id && left.host==right.host && left.port==right.port; }
        else if constexpr(std::is_same_v<T,WireRouting>) { return left.data_type==right.data_type && left.outputs==right.outputs && left.selected==right.selected; }
        else if constexpr(std::is_same_v<T,WireReduction>) { return left.data_type==right.data_type && left.operation==right.operation; }
    },a);
}
bool same_graph(const WireProcessingGraph& a,const WireProcessingGraph& b) {
    if(a.graph_identity!=b.graph_identity || a.processing_version!=b.processing_version ||
        a.nodes.size()!=b.nodes.size() || a.edges.size()!=b.edges.size())return false;
    for(std::size_t i=0;i<a.nodes.size();++i) {
        const auto& x=a.nodes[i]; const auto& y=b.nodes[i];
        if(x.id!=y.id || x.kind!=y.kind || x.enabled!=y.enabled || !same_parameters(x.parameters,y.parameters))return false;
    }
    for(std::size_t i=0;i<a.edges.size();++i) {
        const auto& x=a.edges[i]; const auto& y=b.edges[i];
        if(x.from!=y.from || x.to!=y.to || x.output_port!=y.output_port || x.input_port!=y.input_port)return false;
    }
    return true;
}
bool same_optional_graph(const std::optional<WireProcessingGraph>& a,const std::optional<WireProcessingGraph>& b) {
    return a.has_value()==b.has_value() && (!a || same_graph(*a,*b));
}
bool same_diagnostic(const WireDiagnostic& a,const WireDiagnostic& b) {
    return a.node==b.node && a.parameter_key==b.parameter_key && a.direction==b.direction &&
        a.port==b.port && a.edge_index==b.edge_index;
}
template<class T> bool same_list(std::span<const T> a,std::span<const T> b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin());
}
bool same_capabilities(const WireCapabilities& a,const WireCapabilities& b) {
    return same_list(a.node_kinds,b.node_kinds) && same_list(a.receive_types,b.receive_types) &&
        same_list(a.publish_types,b.publish_types) &&
        a.processing_version==b.processing_version &&
        a.max_wled_outputs==b.max_wled_outputs &&
        a.max_nodes==b.max_nodes &&
        a.max_edges==b.max_edges &&
        a.max_memory_bytes==b.max_memory_bytes &&
        a.max_frame_samples==b.max_frame_samples &&
        a.max_history_frames==b.max_history_frames &&
        a.max_sessions==b.max_sessions &&
        a.max_pending_prepared==b.max_pending_prepared &&
        a.prepared_ttl_ms==b.prepared_ttl_ms &&
        a.max_graph_identity_bytes==b.max_graph_identity_bytes &&
        a.max_prepared_bytes==b.max_prepared_bytes &&
        a.max_field_string_bytes==b.max_field_string_bytes &&
        a.max_replay_bytes_per_session==b.max_replay_bytes_per_session &&
        a.max_replay_bytes_total==b.max_replay_bytes_total &&
        a.max_control_scratch_bytes==b.max_control_scratch_bytes &&
        a.explicit_persist==b.explicit_persist;
}
bool same_message(const WireControlMessage& a,const WireControlMessage& b) {
    if(a.schema_version!=b.schema_version || a.identity.client!=b.identity.client ||
        a.identity.session!=b.identity.session || a.identity.epoch!=b.identity.epoch || a.body.index()!=b.body.index())return false;
    if(const auto* x=std::get_if<WireCommand>(&a.body)) {
        const auto& y=std::get<WireCommand>(b.body);
        return x->id==y.id && x->request_id==y.request_id && x->expected_revision==y.expected_revision &&
            same_optional_graph(x->processing,y.processing) && x->token==y.token && x->graph_identity==y.graph_identity &&
            x->node==y.node && x->enabled==y.enabled && x->graph_revision==y.graph_revision &&
            x->parameters.has_value()==y.parameters.has_value() && (!x->parameters || same_parameters(*x->parameters,*y.parameters));
    }
    const auto& x=std::get<WireReply>(a.body); const auto& y=std::get<WireReply>(b.body);
    return x.request_id==y.request_id && x.accepted==y.accepted && x.error==y.error && x.status==y.status &&
        x.validation_error==y.validation_error && x.validation_node==y.validation_node &&
        x.validation_edge_index==y.validation_edge_index && same_diagnostic(x.validation_location,y.validation_location) &&
        same_diagnostic(x.error_location,y.error_location) && x.state.epoch==y.state.epoch &&
        x.state.revision==y.state.revision && x.state.graph_revision==y.state.graph_revision &&
        x.state.persisted_graph_revision==y.state.persisted_graph_revision && x.state.lifecycle==y.state.lifecycle &&
        x.state.run_requested==y.state.run_requested && same_optional_graph(x.state.applied,y.state.applied) &&
        x.state.own_prepared_token==y.state.own_prepared_token && x.prepared_token==y.prepared_token &&
        x.expires_at_ms==y.expires_at_ms && x.memory.state_bytes==y.memory.state_bytes &&
        x.memory.buffer_bytes==y.memory.buffer_bytes && x.memory.scratch_bytes==y.memory.scratch_bytes &&
        x.capabilities.has_value()==y.capabilities.has_value() && (!x.capabilities || same_capabilities(*x.capabilities,*y.capabilities));
}
struct Arena {
    std::array<WireNode,128> nodes{};
    std::array<WireEdge,256> edges{};
    std::array<char,32768> text{};
    std::array<std::string_view,26> kinds{};
    std::array<WireFrameType,9> receive{},publish{};
    ControlDecodeArena view() {return {nodes,edges,text,kinds,receive,publish};}
};
unsigned matrix_cases=0;
void roundtrip(const WireControlMessage& source) {
    CborControlCodec cbor; ProtobufControlCodec protobuf;
    Arena first,second;
    std::array<std::uint8_t,32768> original{},scratch{},converted{},reencoded{};
    const ControlCodec* codecs[]{&cbor,&protobuf};
    for(unsigned index=0;index<2;++index) {
        auto encoded=codecs[index]->encode(source,original,scratch);
        assert(encoded);
        WireControlMessage decoded;
        auto result=codecs[index]->decode(std::span(original).first(encoded.produced),first.view(),decoded);
        if(!result)std::fprintf(stderr,"matrix decode codec=%u status=%u case=%u\n",index,unsigned(result.status),matrix_cases);
        assert(result && same_message(source,decoded));
        const auto oracle=original;
        original.fill(0); // every decoded graph/capability/network string owns arena storage
        assert(same_message(source,decoded));
        auto cross=codecs[1-index]->encode(decoded,converted,scratch);
        assert(cross);
        WireControlMessage parity;
        result=codecs[1-index]->decode(std::span(converted).first(cross.produced),second.view(),parity);
        assert(result && same_message(source,parity));
        auto back=codecs[index]->encode(parity,reencoded,scratch);
        assert(back && back.produced==encoded.produced && std::memcmp(oracle.data(),reencoded.data(),back.produced)==0);
    }
    ++matrix_cases;
}
void refusal(const ControlCodec& codec,std::span<const std::uint8_t> bytes,ControlDecodeArena view,
    Arena& arena,ControlCodecStatus status,const ControlCodecLimits& limits={}) {
    std::array<unsigned char,sizeof(Arena)> before{};
    std::memcpy(before.data(),&arena,sizeof(arena));
    WireControlMessage output;
    output.identity={999,998,997};
    WireReply sentinel;
    sentinel.request_id=12345;sentinel.state.revision=67890;
    sentinel.error_location.edge_index=0;output.body=sentinel;
    const auto expected=output;
    auto result=codec.decode(bytes,view,output,limits);
    assert(!result && result.status==status && result.consumed==0 && result.produced==0);
    assert(output.identity.client==999 && output.identity.epoch==997);
    assert(same_message(expected,output));
    assert(std::memcmp(before.data(),&arena,sizeof(arena))==0);
}
void resource_matrix(const WireControlMessage& source) {
    CborControlCodec cbor;ProtobufControlCodec protobuf;
    const ControlCodec* codecs[]{&cbor,&protobuf};
    std::array<std::uint8_t,32768> bytes{},scratch{};
    const auto requirements=validate_control_message(source).required_arena;
    assert(requirements.nodes && requirements.edges && requirements.text_bytes && requirements.capability_kinds &&
        requirements.receive_types && requirements.publish_types);
    for(const auto* codec:codecs) {
        auto encoded=codec->encode(source,bytes,scratch); assert(encoded);
        auto payload=std::span(bytes).first(encoded.produced);
        Arena arena;arena.text.fill('Z');arena.nodes[0].id=777;arena.edges[0].from=888;
        for(unsigned resource=0;resource<6;++resource) {
            auto view=arena.view();
            switch(resource) {
            case 0:view.nodes=view.nodes.first(requirements.nodes-1);break;
            case 1:view.edges=view.edges.first(requirements.edges-1);break;
            case 2:view.text=view.text.first(requirements.text_bytes-1);break;
            case 3:view.capability_kinds=view.capability_kinds.first(requirements.capability_kinds-1);break;
            case 4:view.receive_types=view.receive_types.first(requirements.receive_types-1);break;
            case 5:view.publish_types=view.publish_types.first(requirements.publish_types-1);break;
            }
            refusal(*codec,payload,view,arena,ControlCodecStatus::buffer_too_small);
        }
        for(unsigned resource=0;resource<9;++resource) {
            ControlCodecLimits limits;
            switch(resource) {
            case 0:limits.max_nodes=requirements.nodes-1;break;
            case 1:limits.max_edges=requirements.edges-1;break;
            case 2:limits.max_total_string_bytes=requirements.text_bytes-1;break;
            case 3:limits.max_capability_kinds=requirements.capability_kinds-1;break;
            case 4:limits.max_capability_types=requirements.receive_types-1;break;
            case 5:limits.max_wire_bytes=encoded.produced-1;break;
            case 6:limits.max_depth=2;break;
            case 7:limits.max_graph_identity_bytes=1;break;
            case 8:limits.max_field_string_bytes=1;break;
            }
            refusal(*codec,payload,arena.view(),arena,ControlCodecStatus::limit_exceeded,limits);
            bytes.fill(0xa5);
            auto rejected=codec->encode(source,bytes,scratch,limits);
            assert(!rejected && rejected.produced==0);
            for(auto byte:bytes)assert(byte==0xa5);
            encoded=codec->encode(source,bytes,scratch);assert(encoded);
            payload=std::span(bytes).first(encoded.produced);
        }
        // Every cut of a required-tail reply is malformed and atomic.
        for(std::size_t length=0;length<payload.size();++length)
            refusal(*codec,payload.first(length),arena.view(),arena,ControlCodecStatus::malformed);
    }
    // Empty NoParameters is still a container; encode and decode enforce the
    // same conceptual schema depth in both formats at every threshold.
    for(std::size_t depth=0;depth<=10;++depth) {
        bool accepted[2]{};
        for(unsigned index=0;index<2;++index) {
            ControlCodecLimits limits;limits.max_depth=depth;
            auto normal=codecs[index]->encode(source,bytes,scratch);assert(normal);
            Arena arena;WireControlMessage output;
            auto decoded=codecs[index]->decode(std::span(bytes).first(normal.produced),arena.view(),output,limits);
            auto encoded=codecs[index]->encode(source,bytes,scratch,limits);
            assert(bool(decoded)==bool(encoded));
            accepted[index]=bool(encoded);
            if(!encoded)assert(encoded.status==ControlCodecStatus::limit_exceeded && decoded.status==ControlCodecStatus::limit_exceeded);
        }
        assert(accepted[0]==accepted[1]);
    }
}
void malformed_matrix() {
    CborControlCodec cbor;ProtobufControlCodec protobuf;Arena arena;
    constexpr std::uint8_t missing_cbor[]{0xa3,1,1,2,0xa3,1,11,2,12,3,13,3,0xa2,1,2,3,15};
    constexpr std::uint8_t missing_protobuf[]{8,1,18,6,8,11,16,12,24,13,26,4,8,2,24,15};
    refusal(cbor,missing_cbor,arena.view(),arena,ControlCodecStatus::malformed);
    refusal(protobuf,missing_protobuf,arena.view(),arena,ControlCodecStatus::malformed);
    constexpr std::uint8_t multiple_cbor[]{0xa3,1,1,2,0xa3,1,11,2,12,3,13,3,0xa6,1,6,2,14,3,15,7,0,8,0xa2,1,0xa0,2,0xa1,1,0,9,0xf5};
    constexpr std::uint8_t multiple_protobuf[]{8,1,18,6,8,11,16,12,24,13,26,18,8,6,16,14,24,15,56,0,66,6,10,0,18,2,8,0,72,1};
    refusal(cbor,multiple_cbor,arena.view(),arena,ControlCodecStatus::malformed);
    refusal(protobuf,multiple_protobuf,arena.view(),arena,ControlCodecStatus::malformed);
    constexpr std::uint8_t overflow_cbor[]{0xa3,1,0x1b,0,0,0,1,0,0,0,0,2,0xa3,1,11,2,12,3,13,3,0xa3,1,2,2,14,3,15};
    constexpr std::uint8_t overflow_protobuf[]{8,0xff,0xff,0xff,0xff,0x1f,18,6,8,11,16,12,24,13,26,6,8,2,16,14,24,15};
    refusal(cbor,overflow_cbor,arena.view(),arena,ControlCodecStatus::malformed);
    refusal(protobuf,overflow_protobuf,arena.view(),arena,ControlCodecStatus::malformed);
    const ControlCodec* codecs[]{&cbor,&protobuf};
    for(unsigned index=0;index<2;++index) {
        std::array<std::uint8_t,4096> bytes{},scratch{};
        WireControlMessage source;WireCommand command;
        command.id=WireCommandId::set_parameters;command.node=7;command.parameters=WireGain{.25};
        source.body=command;
        auto encoded=codecs[index]->encode(source,bytes,scratch);assert(encoded);
        constexpr std::uint8_t cbor_double[]{0x3f,0xd0,0,0,0,0,0,0};
        constexpr std::uint8_t protobuf_double[]{0,0,0,0,0,0,0xd0,0x3f};
        auto pattern=index==0?std::span(cbor_double):std::span(protobuf_double);
        auto found=std::search(bytes.begin(),bytes.begin()+encoded.produced,pattern.begin(),pattern.end());
        assert(found!=bytes.begin()+encoded.produced);
        constexpr std::uint8_t cbor_nan[]{0x7f,0xf8,0,0,0,0,0,0};
        constexpr std::uint8_t protobuf_nan[]{0,0,0,0,0,0,0xf8,0x7f};
        std::memcpy(&*found,index==0?cbor_nan:protobuf_nan,8);
        refusal(*codecs[index],std::span(bytes).first(encoded.produced),arena.view(),arena,ControlCodecStatus::malformed);
        encoded=codecs[index]->encode(source,bytes,scratch);assert(encoded);
        bytes[encoded.produced-1]=index==0?0xf6:2; // null CBOR / non-binary protobuf bool
        refusal(*codecs[index],std::span(bytes).first(encoded.produced),arena.view(),arena,ControlCodecStatus::malformed);
        command={};command.id=WireCommandId::prepare;
        command.processing=WireProcessingGraph{"graph-id",1,{},{}};source.body=command;
        encoded=codecs[index]->encode(source,bytes,scratch);assert(encoded);
        constexpr std::uint8_t graph_text[]{'g','r','a','p','h','-','i','d'};
        found=std::search(bytes.begin(),bytes.begin()+encoded.produced,std::begin(graph_text),std::end(graph_text));
        assert(found!=bytes.begin()+encoded.produced);
        *found=0xff;
        refusal(*codecs[index],std::span(bytes).first(encoded.produced),arena.view(),arena,ControlCodecStatus::malformed);
        *found=0;
        refusal(*codecs[index],std::span(bytes).first(encoded.produced),arena.view(),arena,ControlCodecStatus::malformed);
        source.schema_version=2;bytes.fill(0xa5);
        auto rejected=codecs[index]->encode(source,bytes,scratch);
        assert(rejected.status==ControlCodecStatus::unsupported_version && rejected.produced==0);
        for(auto byte:bytes)assert(byte==0xa5);
    }
}
void semantic_matrix() {
    forbid_allocations=true;
    std::array<WireParameters,12> parameters{
        WireNoParameters{},WireAudioSource{1},WireMerge{1},WireGain{-0.0},WireMel{17},WireHistory{33},
        WireWindow{2048},WireWindowFunction{3},WireTestSignal{9,96000,123,6789,.25,7,.125},
        WireNetwork{WireFrameType::scalar,"provider-uuid","stream-id","host-name",65535},
        WireRouting{WireFrameType::history,8,7},WireReduction{WireFrameType::stereo_spectrum,2}
    };
    std::array<WireNode,26> nodes{};
    std::array<std::string_view,26> kinds{};
    std::array<WireFrameType,9> types{};
    const auto descriptors=domain::node_descriptors();
    assert(descriptors.size()==nodes.size());
    for(std::size_t i=0;i<nodes.size();++i) {
        auto kind=descriptors[i].kind;
        WireParameters value=WireNoParameters{};
        if(kind==domain::NodeKind::AudioSource)value=parameters[1];
        else if(kind==domain::NodeKind::SpectrumMerge)value=parameters[2];
        else if(kind==domain::NodeKind::SpectrumGain || kind==domain::NodeKind::WLED || kind==domain::NodeKind::WLEDEncode)value=parameters[3];
        else if(kind==domain::NodeKind::Mel)value=parameters[4];
        else if(kind==domain::NodeKind::Spectrogram)value=parameters[5];
        else if(kind==domain::NodeKind::Window)value=parameters[6];
        else if(kind==domain::NodeKind::WindowFunction)value=parameters[7];
        else if(kind==domain::NodeKind::TestSignal)value=parameters[8];
        else if(kind==domain::NodeKind::NetworkInput || kind==domain::NodeKind::NetworkOutput)value=parameters[9];
        else if(kind==domain::NodeKind::Splitter || kind==domain::NodeKind::Switch || kind==domain::NodeKind::Gate)value=parameters[10];
        else if(kind==domain::NodeKind::ScalarReduce)value=parameters[11];
        nodes[i]={static_cast<std::int32_t>(i),descriptors[i].id,value,(i%2)==0};
        kinds[i]=descriptors[i].id;
    }
    for(unsigned i=0;i<9;++i)types[i]=static_cast<WireFrameType>(i);
    std::array<WireEdge,2> edges{WireEdge{INT32_MIN,INT32_MAX,UINT16_MAX,0},WireEdge{0,1,0,UINT16_MAX}};
    WireProcessingGraph graph{"граф-wire",UINT32_MAX,nodes,edges};
    WireControlMessage message;message.identity={UINT64_MAX,17,18};
    for(unsigned id=1;id<=9;++id) {
        WireCommand command;command.id=static_cast<WireCommandId>(id);command.request_id=UINT64_MAX;command.expected_revision=UINT64_MAX;
        if(id==3)command.processing=graph;
        if(id==4 || id==5)command.token=UINT64_MAX;
        if(id==4)command.graph_identity=graph.graph_identity;
        if(id==6){command.node=INT32_MIN;command.enabled=false;command.parameters=parameters[0];}
        if(id==9)command.graph_revision=UINT64_MAX;
        message.body=command;roundtrip(message);
        if(id==6) for(const auto& value:parameters){command.parameters=value;message.body=command;roundtrip(message);}
    }
    for(unsigned id=1;id<=9;++id) {
        WireCommand command;command.id=static_cast<WireCommandId>(id);
        if(id==3)command.processing=WireProcessingGraph{"",1,{},{}};
        if(id==6)command.parameters=WireNoParameters{};
        message.body=command;roundtrip(message);
    }
    const std::array<WireParameters,12> defaults{
        WireNoParameters{},WireAudioSource{},WireMerge{},WireGain{},WireMel{},WireHistory{},
        WireWindow{},WireWindowFunction{},WireTestSignal{},WireNetwork{},WireRouting{},WireReduction{}
    };
    for(const auto& value:defaults) {
        WireCommand command;command.id=WireCommandId::set_parameters;command.parameters=value;
        message.body=command;roundtrip(message);
    }
    WireCapabilities capabilities;
    capabilities.node_kinds=kinds;capabilities.receive_types=types;capabilities.publish_types=types;
    capabilities.processing_version=UINT32_MAX;
    capabilities.max_wled_outputs=102;
    capabilities.max_nodes=103;
    capabilities.max_edges=104;
    capabilities.max_memory_bytes=105;
    capabilities.max_frame_samples=106;
    capabilities.max_history_frames=107;
    capabilities.max_sessions=108;
    capabilities.max_pending_prepared=1;
    capabilities.prepared_ttl_ms=110;
    capabilities.max_graph_identity_bytes=111;
    capabilities.max_prepared_bytes=112;
    capabilities.max_field_string_bytes=113;
    capabilities.max_replay_bytes_per_session=114;
    capabilities.max_replay_bytes_total=115;
    capabilities.max_control_scratch_bytes=116;
    capabilities.explicit_persist=true;
    for(unsigned error=0;error<13;++error) {
        WireReply reply;reply.request_id=UINT64_MAX;reply.accepted=(error==0);reply.error=static_cast<WireControlError>(error);
        reply.status=static_cast<WireStatus>(error%11);reply.validation_error=static_cast<WireGraphError>(error%14);
        reply.validation_node=INT32_MIN;reply.validation_edge_index=UINT64_MAX;
        reply.validation_location={INT32_MIN,"parameter",WireDirection::input,0,0};
        reply.error_location={INT32_MAX,"different-key",WireDirection::output,UINT16_MAX,UINT64_MAX};
        reply.state={19,20,21,22,static_cast<WireLifecycle>(error%4),error%2==0,graph,23};
        reply.prepared_token=UINT64_MAX;reply.expires_at_ms=UINT64_MAX;reply.memory={UINT64_MAX,UINT64_MAX,UINT64_MAX};
        reply.capabilities=capabilities;message.body=reply;roundtrip(message);
    }
    WireReply reply;reply.validation_error=WireGraphError::unsupported_version;
    reply.state.applied=graph;reply.capabilities=capabilities;message.body=reply;
    roundtrip(message);resource_matrix(message);
    reply.state.applied=WireProcessingGraph{"empty",1,{},{}};
    reply.capabilities=WireCapabilities{};message.body=reply;roundtrip(message);
    reply.state.applied.reset();reply.capabilities.reset();message.body=reply;roundtrip(message);
    // Deep malformed input cannot use caller's large limit to exhaust the stack.
    std::array<std::uint8_t,41> deep{};deep.fill(0x81);deep.back()=0;
    CborControlCodec cbor;Arena arena;ControlCodecLimits limits;limits.max_depth=SIZE_MAX;
    refusal(cbor,deep,arena.view(),arena,ControlCodecStatus::limit_exceeded,limits);
    malformed_matrix();
    forbid_allocations=false;
    std::printf("semantic parity cases=%u; arena/limits/truncation/no-allocation matrix passed\n",matrix_cases);
}
} // namespace
int main() {
    semantic_matrix();
    WireControlMessage message;
    message.identity = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    WireCommand command;
    command.request_id = UINT64_MAX;
    command.expected_revision = UINT64_MAX;
    message.body = command;
    assert(validate_control_message(message));
    assert(select_control_codec(3,3,1,1) == ControlCodecId::cbor);
    assert(select_control_codec(3,2,1,1) == ControlCodecId::protobuf);
    assert(!select_control_codec(1,2,1,1));
    assert(!select_control_codec(3,3,1,2));
    assert(!select_control_codec(0x80,0x80,1,1));
    message.schema_version = 2;
    assert(validate_control_message(message).status == ControlCodecStatus::unsupported_version);
    message.schema_version = 1;
    command.id = WireCommandId::prepare;
    message.body = command;
    assert(validate_control_message(message).status == ControlCodecStatus::malformed);

    WireNode node{1,"TestSignal",WireTestSignal{},true};
    WireEdge edge{1,2,65535,0}; // domain semantics remain application responsibility
    command.processing = WireProcessingGraph{"graph-A",1,{&node,1},{&edge,1}};
    message.body = command;
    auto result = validate_control_message(message);
    assert(result && result.required_arena.nodes == 1 && result.required_arena.edges == 1);
    assert(result.required_arena.text_bytes == 7 + 10);
    ControlCodecLimits limits;
    limits.max_nodes = 0;
    assert(validate_control_message(message,limits).status == ControlCodecStatus::limit_exceeded);
    limits = {};
    limits.max_total_string_bytes = 16;
    assert(validate_control_message(message,limits).status == ControlCodecStatus::limit_exceeded);
    node.parameters = WireTestSignal{8,48000,73,13901,std::numeric_limits<double>::infinity(),6,.18};
    assert(validate_control_message(message).status == ControlCodecStatus::malformed);
    node.parameters = WireTestSignal{};
    node.kind = "FFTView";
    assert(validate_control_message(message).status == ControlCodecStatus::malformed);
    node.parameters = WireNoParameters{};
    assert(validate_control_message(message));
    node.kind = std::string_view{"Bad\xC0\x80",5};
    assert(validate_control_message(message).status == ControlCodecStatus::malformed);
    node.kind = "TestSignal";
    node.parameters = WireTestSignal{};
    node.kind = "FutureProcessingKind";
    assert(validate_control_message(message)); // mapper/domain decides support
    node.kind = "TestSignal";
    // Extraneous body data must never disappear silently during encoding.
    command.id = WireCommandId::stop;
    message.body = command;
    assert(validate_control_message(message).status == ControlCodecStatus::malformed);
    WireReply reply;
    reply.request_id = UINT64_MAX;
    reply.state.revision = UINT64_MAX;
    reply.validation_location.edge_index = 0;
    reply.error_location.direction = WireDirection::output;
    reply.error_location.port = 0;
    message.body = reply;
    assert(validate_control_message(message));
    reply.error = static_cast<WireControlError>(255);
    message.body = reply;
    assert(validate_control_message(message).status == ControlCodecStatus::malformed);
    // All registered views carry processing NoParameters, all canonical names retained.
    for (const auto& descriptor : domain::node_descriptors()) {
        auto parameters = domain::parameter_descriptors(descriptor.kind);
        if (descriptor.view) {
            node = {1,descriptor.id,WireNoParameters{},true};
            command.id = WireCommandId::prepare;
            message.body = command;
            assert(validate_control_message(message));
            for (const auto& field : parameters)
                assert(field.classification == domain::ParameterClassification::Presentation);
        }
    }
    std::puts("control wire contract validation/semantic IDs passed");
}
