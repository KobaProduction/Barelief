#include "pipeline/port_delivery_codec.h"
#include "pipeline/stream_codec.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
namespace pipeline::port_delivery {
namespace {
bool utf8(std::string_view s) {
    for(size_t i=0;i<s.size();) {
        const auto c=uint8_t(s[i++]);if(!c)return false;if(c<128)continue;
        unsigned n=0,v=0,min=0;
        if(c>=0xc2 && c<=0xdf){n=1;v=c&31;min=0x80;}
        else if(c>=0xe0 && c<=0xef){n=2;v=c&15;min=0x800;}
        else if(c>=0xf0 && c<=0xf4){n=3;v=c&7;min=0x10000;}else return false;
        if(n>s.size()-i)return false;
        while(n--){const auto d=uint8_t(s[i++]);if((d&0xc0)!=0x80)return false;v=(v<<6)|(d&63);}
        if(v<min || v>0x10ffff || (v>=0xd800 && v<=0xdfff))return false;
    }return true;
}
bool text(std::string_view s,size_t max,bool empty=false){return (empty || !s.empty()) && s.size()<=max && utf8(s);}
bool limits(Limits l){return l.max_message_bytes>=256 && l.max_message_bytes<=2*1024*1024 && l.max_endpoints && l.max_endpoints<=2048;}
bool direction(PortDirection d){return d==PortDirection::Input || d==PortDirection::Output;}
uint8_t wire_direction(PortDirection d){return d==PortDirection::Input?0:1;}
bool endpoint(const EndpointIdentity &e){return text(e.graph_id,256) && e.node>=0 && direction(e.direction);}
struct Reader {
    std::span<const uint8_t> bytes;size_t p=0;bool ok=true;
    uint64_t get(size_t n) {if(n>bytes.size()-p){ok=false;return 0;}uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(bytes[p++])<<(i*8);return v;}
    std::span<const uint8_t> block(size_t n){if(n>bytes.size()-p){ok=false;return {};}auto b=bytes.subspan(p,n);p+=n;return b;}
    std::string_view str(size_t max,bool empty=false){const auto b=block(size_t(get(2)));std::string_view s(reinterpret_cast<const char*>(b.data()),b.size());if(!text(s,max,empty))ok=false;return s;}
};
struct Writer {
    std::span<uint8_t> bytes;size_t p=0;
    void put(uint64_t v,size_t n){for(size_t i=0;i<n;++i)bytes[p++]=uint8_t(v>>(i*8));}
    void block(std::span<const uint8_t> b){if(!b.empty())std::memcpy(bytes.data()+p,b.data(),b.size());p+=b.size();}
    void str(std::string_view s){put(s.size(),2);block({reinterpret_cast<const uint8_t*>(s.data()),s.size()});}
};
Result fail(Status status=Status::Malformed){return {status,0,0};}
Result capacity(size_t n,std::span<uint8_t> out,Limits l,size_t extra_cap=2*1024*1024){
    if(!limits(l) || n>l.max_message_bytes || n>extra_cap)return fail(Status::LimitExceeded);
    return {out.size()<n?Status::LimitExceeded:Status::Accepted,n,0};
}
bool known_type(uint64_t t){return t<=255 && signal_contracts::frame_descriptor(static_cast<FrameType>(t));}
bool geometry(const signal_contracts::PortContract &c) {
    return signal_contracts::frame_descriptor(c.type) && c.sample_rate<=768000 && c.fft_size<=65536 && c.bands<=4096 &&
        c.history_capacity<=2000 && c.columns<=2*1024*1024/4 && std::isfinite(c.frequency_step) && c.frequency_step>=0;
}
bool read_endpoint(Reader &r,EndpointIdentity *out) {
    auto id=r.str(256);const auto node=r.get(4),d=r.get(1),port=r.get(2);
    if(node>INT32_MAX || d>1)r.ok=false;
    if(out && r.ok)*out={std::string(id),int32_t(node),d?PortDirection::Output:PortDirection::Input,uint16_t(port)};
    return r.ok;
}
void put_endpoint(Writer &w,const EndpointIdentity &e){w.str(e.graph_id);w.put(uint32_t(e.node),4);w.put(wire_direction(e.direction),1);w.put(e.port,2);}
// Full no-allocation payload preflight; probe validates all bytes before returning
// buffer_too_small. It never publishes a partially decoded view.
bool payload_valid(std::span<const uint8_t> bytes,FrameType type) {
    if(bytes.size()<bdsp_protocol::StreamHeaderBytes || bytes[2]!=uint8_t(type))return false;
    signal_contracts::FrameView dummy;
    const auto result=bdsp_protocol::decode(bytes,{}, {},{},dummy);
    return result || result.status==signal_contracts::Status::buffer_too_small;
}
uint64_t at(std::span<const uint8_t> b,size_t pos,size_t n){Reader r{b};r.p=pos;return r.get(n);}
bool matching_geometry(std::span<const uint8_t> b,const signal_contracts::PortContract &c,bool known) {
    if(!known)return bool(b[3]&4);
    if(b[3]&4)return true;
    return at(b,60,4)==c.sample_rate && at(b,64,4)==c.fft_size && at(b,68,4)==c.bands &&
        at(b,72,4)==c.history_capacity && at(b,88,4)==(c.type==FrameType::WledPacket?11:c.columns) &&
        std::bit_cast<float>(uint32_t(at(b,76,4)))==c.frequency_step && bool(b[3]&2)==c.bark_weighted;
}
}
Result encode(const Subscribe &v,std::span<uint8_t> out,Limits l) {
    if(!v.nonce || !limits(v.limits) || !v.type_mask || (v.type_mask&~AllTypes))return fail();
    if(v.endpoints.size()>v.limits.max_endpoints || v.endpoints.size()>l.max_endpoints)return fail(Status::LimitExceeded);
    size_t n=22;for(size_t i=0;i<v.endpoints.size();++i){const auto &e=v.endpoints[i];if(!endpoint(e))return fail(Status::InvalidEndpoint);
        for(size_t j=0;j<i;++j)if(v.endpoints[j]==e)return fail(Status::InvalidEndpoint);n+=9+e.graph_id.size();}
    auto result=capacity(n,out,l);if(!result)return result;
    Writer w{out};w.put(Version,2);w.put(0,2);w.put(v.nonce,8);w.put(v.limits.max_message_bytes,4);w.put(v.limits.max_endpoints,2);w.put(v.type_mask,2);w.put(v.endpoints.size(),2);
    for(const auto &e:v.endpoints)put_endpoint(w,e);result.written_bytes=w.p;return result;
}
Result decode(std::span<const uint8_t> bytes,Subscribe &out,Limits l) {
    if(!limits(l) || bytes.size()>l.max_message_bytes || bytes.size()<22)return fail(Status::LimitExceeded);
    Reader r{bytes};if(r.get(2)!=Version)return fail(Status::UnsupportedVersion);if(r.get(2))return fail();
    Subscribe v;v.nonce=r.get(8);v.limits.max_message_bytes=uint32_t(r.get(4));v.limits.max_endpoints=uint16_t(r.get(2));v.type_mask=uint16_t(r.get(2));const auto count=r.get(2);
    if(!v.nonce || !limits(v.limits) || !v.type_mask || (v.type_mask&~AllTypes))return fail();
    if(count>l.max_endpoints || count>v.limits.max_endpoints || count>(bytes.size()-r.p)/9)return fail(Status::LimitExceeded);
    struct Key {std::string_view graph;uint32_t node;uint16_t port;uint8_t direction;};
    std::array<Key,2048> keys{};
    const auto start=r.p;for(size_t i=0;i<count;++i) {
        const auto graph=r.str(256);const auto node=r.get(4),dir=r.get(1),port=r.get(2);
        if(!r.ok || node>INT32_MAX || dir>1)return fail(Status::InvalidEndpoint);
        keys[i]={graph,uint32_t(node),uint16_t(port),uint8_t(dir)};
        for(size_t j=0;j<i;++j)if(keys[j].graph==graph && keys[j].node==node && keys[j].direction==dir && keys[j].port==port)return fail(Status::InvalidEndpoint);
    }
    if(!r.ok || r.p!=bytes.size())return fail();
    // Only after structural preflight. Count/string bytes are bounded by message.
    r.p=start;v.endpoints.resize(count);for(auto &e:v.endpoints)read_endpoint(r,&e);
    out=std::move(v);return {Status::Accepted,bytes.size(),bytes.size()};
}
Result encode(const Ack &v,std::span<uint8_t> out,Limits l) {
    if(uint16_t(v.status)>uint16_t(Status::Malformed) || !v.nonce || !limits(v.limits) || (v.type_mask&~AllTypes) ||
       !text(v.graph_id,256,v.status!=Status::Accepted) || !text(v.error,256,true) || (v.status==Status::Accepted && (!v.session_epoch || !v.type_mask)))return fail();
    auto result=capacity(40+v.graph_id.size()+v.error.size(),out,l,1024);if(!result)return result;
    Writer w{out};w.put(Version,2);w.put(uint16_t(v.status),2);w.put(v.nonce,8);w.put(v.session_epoch,8);w.put(v.graph_revision,8);w.put(v.limits.max_message_bytes,4);w.put(v.limits.max_endpoints,2);w.put(v.type_mask,2);w.str(v.graph_id);w.str(v.error);result.written_bytes=w.p;return result;
}
Result decode(std::span<const uint8_t> bytes,Ack &out,Limits l) {
    if(!limits(l) || bytes.size()>std::min(size_t(l.max_message_bytes),size_t(1024)) || bytes.size()<40)return fail(Status::LimitExceeded);
    Reader r{bytes};if(r.get(2)!=Version)return fail(Status::UnsupportedVersion);Ack v;const auto status=r.get(2);if(status>uint16_t(Status::Malformed))return fail();v.status=Status(status);
    v.nonce=r.get(8);v.session_epoch=r.get(8);v.graph_revision=r.get(8);v.limits.max_message_bytes=uint32_t(r.get(4));v.limits.max_endpoints=uint16_t(r.get(2));v.type_mask=uint16_t(r.get(2));
    const auto graph=r.str(256,v.status!=Status::Accepted),error=r.str(256,true);
    if(!r.ok || r.p!=bytes.size() || !v.nonce || !limits(v.limits) || (v.type_mask&~AllTypes) || (v.status==Status::Accepted && (!v.session_epoch || !v.type_mask)))return fail();
    v.graph_id=graph;v.error=error;out=std::move(v);return {Status::Accepted,bytes.size(),bytes.size()};
}
Result encode(const Frames &v,std::span<uint8_t> out,Limits l) {
    if(!v.nonce || !v.session_epoch || !v.sequence || !text(v.graph_id,256))return fail();
    if(v.entries.size()>l.max_endpoints)return fail(Status::LimitExceeded);
    size_t n=48+v.graph_id.size();
    for(size_t i=0;i<v.entries.size();++i) {
        const auto &e=v.entries[i];const auto kind=domain::kind_id(e.kind);
        if(!endpoint(e.endpoint) || e.endpoint.graph_id!=v.graph_id || e.delivery.epoch!=v.delivery.epoch || e.delivery.graph_revision!=v.delivery.graph_revision ||
           !text(kind,64) || !domain::node_descriptor(e.kind) || !geometry(e.contract) || e.frame.type!=e.contract.type || (!e.contract_known && e.frame.available) || !stream::valid(e.frame))return fail();
        for(size_t j=0;j<i;++j)if(v.entries[j].endpoint==e.endpoint)return fail(Status::InvalidEndpoint);
        const auto payload=stream::payload_size(e.frame);const auto overhead=40+kind.size();
        if(payload>l.max_message_bytes || n>l.max_message_bytes || overhead>l.max_message_bytes-n || payload>l.max_message_bytes-n-overhead)return fail(Status::LimitExceeded);
        n+=overhead+payload;
    }
    auto result=capacity(n,out,l);if(!result)return result;
    // Preflight all full payloads before writing destination; encode is an owning
    // desktop serialization boundary, never the prepared runtime callback.
    std::vector<std::vector<uint8_t>> payloads;payloads.reserve(v.entries.size());
    for(const auto &e:v.entries){auto b=stream::encode(e.frame);if(!matching_geometry(b,e.contract,e.contract_known))return fail();payloads.push_back(std::move(b));}
    Writer w{out};w.put(Version,2);w.put((v.snapshot?1:0)|(v.gap?2:0),2);w.put(v.nonce,8);w.put(v.session_epoch,8);w.put(v.delivery.epoch,8);w.put(v.delivery.graph_revision,8);w.put(v.sequence,8);w.str(v.graph_id);w.put(v.entries.size(),2);
    for(size_t i=0;i<v.entries.size();++i) {
        const auto &e=v.entries[i];const auto &c=e.contract;w.put(uint32_t(e.endpoint.node),4);w.put(wire_direction(e.endpoint.direction),1);w.put(e.endpoint.port,2);w.str(domain::kind_id(e.kind));w.put((e.contract_known?1:0)|(e.window_emitted?2:0),1);w.put(uint8_t(c.type),1);
        w.put(c.sample_rate,4);w.put(c.fft_size,4);w.put(c.bands,4);w.put(c.history_capacity,4);w.put(c.columns,4);w.put(std::bit_cast<uint32_t>(c.frequency_step),4);w.put(c.bark_weighted?1:0,1);w.put(payloads[i].size(),4);w.block(payloads[i]);
    }
    result.written_bytes=w.p;return result;
}
Result decode(std::span<const uint8_t> bytes,Frames &out,Limits l) {
    if(!limits(l) || bytes.size()>l.max_message_bytes || bytes.size()<48)return fail(Status::LimitExceeded);
    Reader r{bytes};if(r.get(2)!=Version)return fail(Status::UnsupportedVersion);const auto flags=r.get(2);if(flags>3)return fail();
    Frames v;v.snapshot=flags&1;v.gap=flags&2;v.nonce=r.get(8);v.session_epoch=r.get(8);v.delivery.epoch=r.get(8);v.delivery.graph_revision=r.get(8);v.sequence=r.get(8);
    auto graph=r.str(256);const auto count=r.get(2);
    if(!v.nonce || !v.session_epoch || !v.sequence || !r.ok)return fail();
    if(count>l.max_endpoints || count>(bytes.size()-r.p)/40)return fail(Status::LimitExceeded);
    const auto start=r.p;
    struct Key {uint32_t node;uint16_t port;uint8_t direction;};
    std::array<Key,2048> keys{};
    const auto pass=[&](bool materialize)->bool {
        for(size_t i=0;i<count;++i) {
            DeliveredPortFrame e;const auto node=r.get(4),d=r.get(1),port=r.get(2);auto kind=r.str(64);const auto ef=r.get(1),type=r.get(1);
            const auto descriptor=std::find_if(domain::node_descriptors().begin(),domain::node_descriptors().end(),[&](const auto &x){return x.id==kind;});
            if(node>INT32_MAX || d>1 || ef>3 || !known_type(type) || descriptor==domain::node_descriptors().end()){r.ok=false;return false;}
            if(!materialize) {
                keys[i]={uint32_t(node),uint16_t(port),uint8_t(d)};
                for(size_t j=0;j<i;++j)if(keys[j].node==node && keys[j].port==port && keys[j].direction==d){r.ok=false;return false;}
            }
            e.contract.type=FrameType(type);e.contract.sample_rate=r.get(4);e.contract.fft_size=r.get(4);e.contract.bands=r.get(4);e.contract.history_capacity=r.get(4);e.contract.columns=r.get(4);e.contract.frequency_step=std::bit_cast<float>(uint32_t(r.get(4)));const auto cf=r.get(1);e.contract.bark_weighted=cf&1;const auto payload=r.block(size_t(r.get(4)));
            if(!r.ok || cf>1 || !geometry(e.contract) || !payload_valid(payload,e.contract.type) || !matching_geometry(payload,e.contract,bool(ef&1))){r.ok=false;return false;}
            if(materialize) {
                e.endpoint={std::string(graph),int32_t(node),d?PortDirection::Output:PortDirection::Input,uint16_t(port)};e.kind=descriptor->kind;e.delivery=v.delivery;e.contract_known=ef&1;e.window_emitted=ef&2;
                std::string error;if(!stream::decode(payload,e.frame,error)){r.ok=false;return false;}v.entries[i]=std::move(e);
            }
        }return r.ok;
    };
    if(!pass(false) || r.p!=bytes.size())return fail();
    r.p=start;v.graph_id=graph;v.entries.resize(count);if(!pass(true))return fail();
    out=std::move(v);return {Status::Accepted,bytes.size(),bytes.size()};
}
bool valid_bootstrap_binding(const PortBinding &binding) noexcept {
    domain::NodeDefinition node;node.kind=binding.kind;node.parameters=domain::default_parameters(node.kind);
    const bool input=binding.endpoint.direction==PortDirection::Input;
    if(input && domain::node_role(node.kind)!=domain::NodeRole::Sink)return false;
    // Routing output count and data type are parameter-dependent; no default count/type is imposed.
    const bool routing=node.kind==domain::NodeKind::Splitter || node.kind==domain::NodeKind::Switch || node.kind==domain::NodeKind::Gate;
    if(routing && !input) return binding.endpoint.port<(node.kind==domain::NodeKind::Gate?1:8) && signal_contracts::frame_descriptor(binding.type);
    const auto type=input?domain::input_type(node,binding.endpoint.port):domain::output_type(node,binding.endpoint.port);
    if(!type)return false;
    const bool dependent=node.kind==domain::NodeKind::NetworkInput || node.kind==domain::NodeKind::NetworkOutput;
    return dependent?bool(signal_contracts::frame_descriptor(binding.type)):binding.type==*type;
}
Result validate_admitted_membership(const Frames &frames,std::span<const PortBinding> admitted) noexcept {
    if(admitted.size()>2048 || frames.entries.size()>2048 || (frames.snapshot && frames.entries.size()!=admitted.size()))return {Status::InvalidEndpoint};
    for(size_t i=0;i<admitted.size();++i) {
        if(admitted[i].endpoint.graph_id!=frames.graph_id)return {Status::InvalidEndpoint};
        for(size_t j=0;j<i;++j)if(admitted[j].endpoint==admitted[i].endpoint)return {Status::InvalidEndpoint};
    }
    for(size_t i=0;i<frames.entries.size();++i) {
        const auto &entry=frames.entries[i];
        const auto found=std::find_if(admitted.begin(),admitted.end(),[&](const auto &binding){return binding.endpoint==entry.endpoint;});
        if(found==admitted.end() || entry.kind!=found->kind || entry.frame.type!=found->type || (found->unavailable_only && entry.frame.available))return {Status::InvalidEndpoint};
        for(size_t j=0;j<i;++j)if(frames.entries[j].endpoint==entry.endpoint)return {Status::InvalidEndpoint};
    }
    return {};
}

}
