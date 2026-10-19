#include <bdsp_protocol/stream_fragments.h>
#include <array>
#include <cassert>
#include <iostream>
#include <vector>
using namespace bdsp_protocol;
using signal_contracts::Status;
namespace {
std::vector<uint8_t> sample_frame() {
    std::vector<uint8_t> frame(150123);
    for(size_t i=0;i<frame.size();++i)frame[i]=i%256;
    return frame;
}
void standard_fragments(const std::vector<uint8_t>& frame,FrameAssembler& assembler,
    std::array<uint8_t,60008>& fragment) {
    std::span<const uint8_t> complete;
    for(size_t off=0;off<frame.size();off+=StreamFragmentBytes) {
        auto result=encode_fragment(frame,off,fragment);assert(result);
        if(off==0) {
            assert(fragment[0]==0x6b && fragment[1]==0x4a && fragment[2]==2 && fragment[3]==0);
            assert(fragment[4]==0 && fragment[5]==0 && fragment[6]==0 && fragment[7]==0);
        }
        auto status=assembler.accept(std::span(fragment).first(result.written_bytes),complete);
        assert(status==(off+StreamFragmentBytes>=frame.size()?Status::ok:Status::would_block));
    }
    assert(complete.size()==frame.size() && std::equal(complete.begin(),complete.end(),frame.begin()));
}
void malformed_fragments(const std::vector<uint8_t>& frame,FrameAssembler& assembler,
    std::array<uint8_t,60008>& fragment) {
    std::span<const uint8_t> complete;
    encode_fragment(frame,0,fragment);assert(assembler.accept(fragment,complete)==Status::would_block);
    encode_fragment(frame,60000,fragment);fragment[4]=0xff;
    assert(assembler.accept(fragment,complete)==Status::malformed && complete.empty());
    assert(assembler.accept(fragment,complete)==Status::malformed);
    std::array<uint8_t,100> tiny{};FrameAssembler bounded(tiny);
    encode_fragment(frame,0,fragment);assert(bounded.accept(fragment,complete)==Status::buffer_too_small);
    std::array<uint8_t,8> short_buffer{};
    auto short_result=encode_fragment(frame,0,short_buffer);
    assert(short_result.status==Status::buffer_too_small && short_result.required_bytes==60008);
    assert(!encode_fragment(frame,frame.size(),fragment));
}
void compact_uart_chunks(const std::vector<uint8_t>& frame,FrameAssembler& assembler,
    std::array<uint8_t,60008>& fragment) {
    std::span<const uint8_t> complete;
    for(const auto chunk:{std::size_t(1016),std::size_t(503)}) {
        assembler.reset();complete={};
        for(std::size_t off=0;off<frame.size();off+=chunk) {
            const auto encoded=encode_fragment(frame,off,fragment,chunk);assert(encoded);
            assert(encoded.written_bytes==8+std::min(chunk,frame.size()-off));
            const auto status=assembler.accept(std::span(fragment).first(encoded.written_bytes),complete);
            assert(status==(off+chunk>=frame.size()?Status::ok:Status::would_block));
        }
        assert(complete.size()==frame.size()&&std::equal(complete.begin(),complete.end(),frame.begin()));
        const auto first=encode_fragment(frame,0,fragment,chunk);assert(first);
        assert(assembler.accept(std::span(fragment).first(first.written_bytes),complete)==Status::would_block);
        assembler.reset();const auto second=encode_fragment(frame,chunk,fragment,chunk);assert(second);
        assert(assembler.accept(std::span(fragment).first(second.written_bytes),complete)==Status::malformed);
    }
}
}
int main() {
    const auto frame=sample_frame();
    std::vector<uint8_t> storage(frame.size());FrameAssembler assembler(storage);
    std::array<uint8_t,60008> fragment{};
    standard_fragments(frame,assembler,fragment);
    malformed_fragments(frame,assembler,fragment);
    compact_uart_chunks(frame,assembler,fragment);
    std::cout<<"BDSP frame fragment wire, desktop/compact UART chunks and bounded reset passed\n";
}
