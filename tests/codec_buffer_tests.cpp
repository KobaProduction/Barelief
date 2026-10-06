#include "pipeline/stream_codec.h"
#include <cassert>
#include <array>
#include <iostream>
#include <limits>
#include <string_view>
#include <cstdlib>
#include <new>
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
int main() {
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
