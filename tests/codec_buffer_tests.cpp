#include "pipeline/stream_codec.h"
#include "pipeline/port_delivery_codec.h"
#include <cassert>
#include <array>
#include <iostream>
#include <limits>
#include <string_view>
#include <cstdlib>
#include <new>
#include <algorithm>
static bool count_allocations=false;
static size_t allocations=0;
void *operator new(size_t n) {if(count_allocations)++allocations;if(auto *p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void *operator new[](size_t n) {return ::operator new(n);}
void operator delete(void *p) noexcept {std::free(p);}
void operator delete[](void *p) noexcept {std::free(p);}
void operator delete(void *p,size_t) noexcept {std::free(p);}
void operator delete[](void *p,size_t) noexcept {std::free(p);}
using namespace pipeline;
static std::vector<uint8_t> hex(std::string_view s) {
    std::vector<uint8_t> out;
    auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(size_t i=0;i<s.size();i+=2) out.push_back(uint8_t(nibble(s[i])*16+nibble(s[i+1])));
    return out;
}
static void admitted_membership() {
    using namespace port_delivery;
    const std::array<PortBinding,3> admitted{{{{"main",1,PortDirection::Output,0},domain::NodeKind::Spectrogram,FrameType::History},{{"main",1,PortDirection::Output,1},domain::NodeKind::Spectrogram,FrameType::Scalar},{{"main",2,PortDirection::Input,0},domain::NodeKind::ScalarView,FrameType::Scalar}}};
    Frames batch;batch.graph_id="main";batch.snapshot=true;for(const auto &binding:admitted){DeliveredPortFrame value;value.endpoint=binding.endpoint;value.kind=binding.kind;value.frame.type=binding.type;value.frame.available=false;batch.entries.push_back(value);}
    assert(validate_admitted_membership(batch,admitted));auto delta=batch;delta.snapshot=false;delta.entries.resize(1);assert(validate_admitted_membership(delta,admitted));
    auto invalid=delta;invalid.entries[0].endpoint.node=99;invalid.sequence=999;invalid.delivery={999,999};assert(!validate_admitted_membership(invalid,admitted));
    invalid=delta;invalid.entries[0].endpoint.direction=PortDirection::Input;assert(!validate_admitted_membership(invalid,admitted));
    invalid=delta;invalid.entries[0].frame.type=FrameType::Scalar;assert(!validate_admitted_membership(invalid,admitted));
    invalid=delta;invalid.entries[0].kind=domain::NodeKind::Gate;assert(!validate_admitted_membership(invalid,admitted));
    invalid=delta;invalid.entries[0].endpoint.port=3;assert(!validate_admitted_membership(invalid,admitted));
    invalid=delta;invalid.entries[0].endpoint.graph_id="other";assert(!validate_admitted_membership(invalid,admitted));
    invalid=batch;invalid.entries.pop_back();assert(!validate_admitted_membership(invalid,admitted));
    invalid=batch;invalid.entries[1]=invalid.entries[0];assert(!validate_admitted_membership(invalid,admitted));
    auto retired=admitted;retired[0].unavailable_only=true;assert(validate_admitted_membership(delta,retired));delta.entries[0].frame.available=true;assert(!validate_admitted_membership(delta,retired));
    assert(batch.entries.size()==3 && batch.sequence==0 && batch.delivery==DeliveryStamp{});assert(validate_admitted_membership(batch,admitted));
    Frames empty;empty.graph_id="main";empty.snapshot=true;assert(validate_admitted_membership(empty,{}));assert(!validate_admitted_membership(batch,{}));
}
static void consolidated_contracts() {
    using namespace port_delivery;
    assert(stream::framed_packet_bound(60008)==60283 && stream::framed_packet_bound(60023)==60298);
    for(size_t bytes:{size_t(0),size_t(1),size_t(65535),size_t(65536),size_t(120005),stream::MaxFrameBytes}) {
        size_t sum=0;for(size_t offset=0;offset<bytes;offset+=60000)sum+=stream::framed_packet_bound(std::min(size_t(60000),bytes-offset)+8);
        assert(stream::fragmented_message_bound(bytes,8)==sum);
        assert(stream::fragmented_message_bound(bytes,23,true)==(bytes<=65535?stream::framed_packet_bound(bytes):stream::fragmented_message_bound(bytes,23)));
    }
    assert(stream::fragmented_message_bound(stream::MaxFrameBytes,8)>stream::MaxFrameBytes);
    assert(!stream::peer_budget_fits(0,0,0,stream::fragmented_message_bound(stream::MaxFrameBytes,8)));
    assert(stream::peer_budget_fits(100,200,300,stream::MaxFrameBytes-600));assert(!stream::peer_budget_fits(100,200,301,stream::MaxFrameBytes-600));
    assert(!stream::peer_budget_fits(SIZE_MAX,1,1,1) && !stream::peer_budget_fits(0,SIZE_MAX,0,0));
    assert(stream::framed_packet_bound(SIZE_MAX)==stream::InvalidByteBound);
    assert(!valid_bootstrap_binding({{"main",1,PortDirection::Output,1},domain::NodeKind::Spectrogram,FrameType::History}));
    assert(valid_bootstrap_binding({{"main",1,PortDirection::Output,1},domain::NodeKind::Spectrogram,FrameType::Scalar}));
    assert(!valid_bootstrap_binding({{"main",1,PortDirection::Input,0},domain::NodeKind::ScalarView,FrameType::History}));
    assert(valid_bootstrap_binding({{"main",1,PortDirection::Input,0},domain::NodeKind::ScalarView,FrameType::Scalar}));
    assert(valid_bootstrap_binding({{"main",1,PortDirection::Output,7},domain::NodeKind::Splitter,FrameType::WledPacket}));
    assert(!valid_bootstrap_binding({{"main",1,PortDirection::Output,8},domain::NodeKind::Splitter,FrameType::Mel}));
    assert(valid_bootstrap_binding({{"main",1,PortDirection::Output,0},domain::NodeKind::NetworkInput,FrameType::Scalar}));
    Frames batch;batch.snapshot=true;batch.graph_id="main";batch.nonce=1;batch.session_epoch=2;batch.sequence=1;batch.delivery={1,1};
    DeliveredPortFrame value;value.endpoint={"main",1,PortDirection::Output,1};value.kind=domain::NodeKind::Spectrogram;value.delivery=batch.delivery;value.contract.type=FrameType::Scalar;value.contract.columns=1;value.contract.sample_rate=48000;value.contract_known=true;value.frame.type=FrameType::Scalar;value.frame.sample_rate=48000;value.frame.sequence=1;value.frame.values={1};batch.entries={value};
    std::array<uint8_t,1024> bytes{};const auto encoded=encode(batch,bytes);assert(encoded);
    batch.entries[0].contract_known=false;std::array<uint8_t,1024> destination;destination.fill(77);assert(!encode(batch,destination) && destination[0]==77);
    auto get16=[&](size_t pos){return size_t(bytes[pos])|(size_t(bytes[pos+1])<<8);};const auto start=44+2+get16(44)+2;const auto flags=start+4+1+2+2+get16(start+7);bytes[flags]&=~1;
    Frames decoded;decoded.sequence=999;count_allocations=true;allocations=0;assert(!decode(std::span(bytes).first(encoded.written_bytes),decoded));count_allocations=false;assert(allocations==0 && decoded.sequence==999);
    batch.entries[0].frame.available=false;batch.entries[0].frame.sequence=0;batch.entries[0].frame.values.clear();
    const auto unavailable=encode(batch,bytes);assert(unavailable);assert(decode(std::span(bytes).first(unavailable.written_bytes),decoded) && !decoded.entries[0].contract_known && !decoded.entries[0].frame.available);
    std::array<float,2> rows{1,2};signal_contracts::FrameView history;history.type=FrameType::History;history.metadata.sequence=1;history.metadata.sample_rate=48000;history.metadata.bands=1;history.metadata.history_capacity=2;history.history={rows,1,2,2,0};
    const auto h=bdsp_protocol::encode(history,bytes);assert(h);history.metadata.history_capacity=1;assert(!bdsp_protocol::encode(history,destination));
    bytes[72]=1;std::array<float,2> samples{77,77};std::array<char,1> id{'x'};signal_contracts::FrameView out;out.metadata.sequence=999;
    assert(!bdsp_protocol::decode(std::span(bytes).first(h.written_bytes),samples,id,{},out) && out.metadata.sequence==999 && samples[0]==77 && id[0]=='x');
    TypedFrame owned;owned.sequence=999;std::string error;count_allocations=true;allocations=0;assert(!stream::decode(std::span(bytes).first(h.written_bytes),owned,error));count_allocations=false;assert(owned.sequence==999); // error string may allocate; no payload ownership was admitted.
    history.metadata.history_capacity=2;const auto correct=bdsp_protocol::encode(history,bytes);assert(correct);bytes[68]=2;
    out.metadata.sequence=999;assert(!bdsp_protocol::decode(std::span(bytes).first(correct.written_bytes),samples,id,{},out) && out.metadata.sequence==999);
    signal_contracts::FrameMetadata unspecified;unspecified.available=true;assert(!bdsp_protocol::compatible_geometry(FrameType::History,unspecified,1,2001));
    history.metadata.bands=0;history.metadata.history_capacity=0;assert(bdsp_protocol::encode(history,bytes)); // characterized missing legacy geometry remains valid.
    for(const auto type:{FrameType::Window,FrameType::Windowed,FrameType::StereoSpectrum,FrameType::Spectrum,FrameType::Mel}) {
        signal_contracts::FrameMetadata metadata;metadata.available=true;metadata.fft_size=8;metadata.bands=8;
        const auto columns=type==FrameType::Mel?8:type==FrameType::Window||type==FrameType::Windowed?8:4;
        assert(bdsp_protocol::compatible_geometry(type,metadata,columns,1));assert(!bdsp_protocol::compatible_geometry(type,metadata,columns+1,1));
    }
}
int main() {
    admitted_membership();
    consolidated_contracts();
    {
        port_delivery::Subscribe request;request.nonce=1;
        request.endpoints.push_back({"ports",1,PortDirection::Output,1});
        std::array<uint8_t,1024> bytes{};
        const auto encoded=port_delivery::encode(request,bytes);
        assert(encoded && encoded.written_bytes>0);
        port_delivery::Subscribe decoded;
        assert(port_delivery::decode(std::span(bytes).first(encoded.written_bytes),decoded));
        assert(decoded.nonce==1 && decoded.endpoints==request.endpoints);
        auto malformed=bytes;malformed[0]=2;
        decoded.nonce=99;
        assert(!port_delivery::decode(std::span(malformed).first(encoded.written_bytes),decoded) && decoded.nonce==99);
        auto replay=request;replay.nonce=0;
        assert(!port_delivery::encode(replay,bytes));
        port_delivery::Ack ack;ack.nonce=1;ack.session_epoch=5;ack.graph_revision=9;ack.graph_id="ports";
        const auto ack_size=port_delivery::encode(ack,bytes);
        assert(ack_size && ack_size.required_bytes==45 && ack_size.written_bytes==45);
        port_delivery::Ack got_ack;
        assert(port_delivery::decode(std::span(bytes).first(ack_size.written_bytes),got_ack));
        assert(got_ack.session_epoch==5 && got_ack.graph_revision==9);
        port_delivery::Frames batch;batch.snapshot=true;batch.nonce=1;batch.session_epoch=5;batch.sequence=1;
        batch.graph_id="ports";batch.delivery={9,11};
        DeliveredPortFrame value;value.endpoint={"ports",1,PortDirection::Output,1};value.delivery=batch.delivery;
        value.kind=domain::NodeKind::Spectrogram;value.contract_known=true;value.contract.type=FrameType::Scalar;
        value.contract.columns=1;value.contract.sample_rate=48000;
        value.frame.type=FrameType::Scalar;value.frame.sample_rate=48000;value.frame.sequence=1;
        value.frame.epoch=700;value.frame.graph_revision=800;value.frame.producer_epoch=701;value.frame.values={0};
        batch.entries.push_back(value);
        auto frame_size=port_delivery::encode(batch,bytes);assert(frame_size && frame_size.written_bytes==frame_size.required_bytes);
        port_delivery::Frames frames;
        assert(port_delivery::decode(std::span(bytes).first(frame_size.written_bytes),frames));
        assert(frames.entries.size()==1 && frames.entries[0].endpoint.port==1 && frames.entries[0].frame.values[0]==0);
        assert(frames.entries[0].frame.graph_revision==800 && frames.delivery.graph_revision==9 && frames.entries[0].frame.epoch==700);
        bytes[3]=128;frames.nonce=99;
        count_allocations=true;allocations=0;
        assert(!port_delivery::decode(std::span(bytes).first(frame_size.written_bytes),frames));
        count_allocations=false;assert(allocations==0 && frames.nonce==99);
        batch.entries.push_back(value);assert(!port_delivery::encode(batch,bytes));batch.entries.pop_back();
        batch.entries[0].endpoint.direction=PortDirection::Input;batch.entries[0].kind=domain::NodeKind::ScalarView;
        frame_size=port_delivery::encode(batch,bytes);assert(frame_size);
        assert(port_delivery::decode(std::span(bytes).first(frame_size.written_bytes),frames));
        assert(frames.entries[0].endpoint.direction==PortDirection::Input);
        batch.entries[0].frame.available=false;batch.entries[0].frame.sequence=0;batch.entries[0].frame.values.clear();
        frame_size=port_delivery::encode(batch,bytes);assert(frame_size);
        assert(port_delivery::decode(std::span(bytes).first(frame_size.written_bytes),frames) && !frames.entries[0].frame.available);
        std::vector<uint8_t> too_big(2*1024*1024+1);
        count_allocations=true;allocations=0;assert(!port_delivery::decode(too_big,frames));count_allocations=false;assert(allocations==0);
    }
    {
        TypedFrame packet;packet.type=FrameType::WledPacket;packet.sequence=1;packet.sample_rate=48000;
        packet.packet.assign(44,0);std::copy_n("00002",6,packet.packet.begin());
        const auto bytes=stream::encode(packet);
        std::array<uint8_t,44> storage;storage.fill(77);
        signal_contracts::FrameView decoded;decoded.metadata.sequence=99;
        auto result=bdsp_protocol::decode(bytes,{}, {},std::span(storage).first(43),decoded);
        assert(result.status==signal_contracts::Status::buffer_too_small && result.required_packet_bytes==44);
        assert(result.required_samples==0 && storage[0]==77 && decoded.metadata.sequence==99);
        assert(bdsp_protocol::decode(bytes,{}, {},storage,decoded));
        assert(decoded.packet.data()==storage.data() && storage[0]=='0' && decoded.metadata.sequence==1);
    }
    TypedFrame f; f.type=FrameType::Mel; f.sample_rate=1; f.sequence=1; f.values={1};
    const auto golden=hex("020005000000000000000000000000000000000000000000000000000100000000000000000000000000000000"
        "000000000000000000000000000000010000000000000000000000000000000000000000000000000000000100"
        "000001000000000000000000000000000000000000000000803f");
    assert(stream::encode(f)==golden);
    signal_contracts::FrameView portable;portable.type=f.type;portable.metadata.sequence=1;
    portable.metadata.sample_rate=1;portable.values=f.values;
    std::vector<uint8_t> into(golden.size(),99);
    auto encoded_result=stream::encode(portable, std::span(into).first(into.size()-1));
    assert(encoded_result.status==signal_contracts::Status::buffer_too_small);
    assert(encoded_result.required_bytes==golden.size() && encoded_result.written_bytes==0 && into[0]==99);
    assert(stream::encode(portable,into) && into==golden);
    std::array<float,1> samples{-77};std::array<char,256> id{};
    signal_contracts::FrameView result_view;result_view.metadata.sequence=77;
    auto decoded_result=stream::decode(golden,{},id,result_view);
    assert(decoded_result.status==signal_contracts::Status::buffer_too_small && decoded_result.required_samples==1);
    assert(samples[0]==-77 && result_view.metadata.sequence==77);
    assert(stream::decode(golden,samples,id,result_view) && samples[0]==1 && result_view.values[0]==1);
    count_allocations=true;
    for(int i=0;i<100;++i) {
        assert(stream::encode(portable,into));
        assert(stream::decode(into,samples,id,result_view));
    }
    count_allocations=false;assert(allocations==0);
    auto invalid=golden;invalid[112]=0;invalid[113]=0;invalid[114]=128;invalid[115]=127;
    samples[0]=-77;assert(!stream::decode(invalid,samples,id,result_view) && samples[0]==-77);
    for(int t=0;t<7;++t) {
        TypedFrame x; x.type=FrameType(t); x.sample_rate=48000; x.sequence=17;
        x.timestamp_ns=-123; x.stream_id="test"; x.source_sequence=99; x.producer_epoch=7;
        if(t<4){x.left={1,2,3};x.right={4,5,6};}
        else if(t<6)x.values={1,2,3}; else x.history={{1,2,3},{4,5,6}};
        auto bytes=stream::encode(x); TypedFrame y; std::string error;
        std::array<float,16> buffer{};std::array<char,256> stream_id{};
        signal_contracts::FrameView borrowed;
        assert(stream::decode(bytes,buffer,stream_id,borrowed));
        assert(borrowed.metadata.stream_id=="test" && borrowed.metadata.timestamp_ns==-123);
        std::vector<uint8_t> recoded(bytes.size());assert(stream::encode(borrowed,recoded) && recoded==bytes);
        assert(stream::decode(bytes,y,error));
        assert(y.left==x.left && y.right==x.right && y.values==x.values && y.history==x.history);
        assert(y.timestamp_ns==-123 && y.source_sequence==99 && y.producer_epoch==7);
        bytes[0]=3; assert(!stream::decode(bytes,y,error) && error=="Unsupported stream version");
        bytes[0]=2; bytes[100]=1; assert(!stream::decode(bytes,y,error) && error=="Reserved fields");
    }
    TypedFrame marker; marker.available=false; marker.type=FrameType::History;
    marker.stream_id="history"; TypedFrame decoded; std::string error;
    assert(stream::decode(stream::encode(marker),decoded,error) && !decoded.available);
    auto marker_bytes=stream::encode(marker);
    assert(stream::decode(marker_bytes,{},id,result_view) && !result_view.metadata.available && result_view.values.empty());
    std::array<float,6> ring{1,2,3,4,5,6};std::array<float,3> newest_zero{};
    signal_contracts::FrameView gated;gated.type=FrameType::History;
    gated.metadata.sequence=1;gated.metadata.sample_rate=48000;gated.metadata.bands=3;gated.metadata.history_capacity=2;
    gated.history={ring,3,2,2,1};gated.history.newest_override=newest_zero;
    TypedFrame expected_history;expected_history.type=FrameType::History;expected_history.sequence=1;
    expected_history.sample_rate=48000;expected_history.bands=3;expected_history.history_capacity=2;
    expected_history.history={{4,5,6},{0,0,0}};
    const auto expected_bytes=stream::encode(expected_history);std::vector<uint8_t> overridden(expected_bytes.size(),77);
    assert(stream::encode(gated,overridden) && overridden==expected_bytes && ring[0]==1);
    assert(stream::decode(overridden,samples,id,result_view).status==signal_contracts::Status::buffer_too_small);
    std::array<float,6> decoded_history{};
    assert(stream::decode(overridden,decoded_history,id,result_view));
    assert(result_view.history.row(0)[0]==4 && result_view.history.row(1)[2]==0 && result_view.history.newest_override.empty());
    gated.history.newest_override=std::span(newest_zero).first(1);overridden[0]=77;
    assert(!stream::encode(gated,overridden) && overridden[0]==77);
    gated.history.newest_override=newest_zero;newest_zero[0]=std::numeric_limits<float>::infinity();
    assert(!stream::encode(gated,overridden) && overridden[0]==77);newest_zero[0]=0;
    portable.history.newest_override=newest_zero;assert(!stream::encode(portable,into));
    gated.metadata.available=false;gated.metadata.sequence=0;gated.history={};gated.history.newest_override=newest_zero;
    assert(!stream::encode(gated,overridden));
    std::cout<<"owned codec characterization passed\n";
}
