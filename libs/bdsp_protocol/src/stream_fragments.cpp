#include "bdsp_protocol/stream_fragments.h"
#include <algorithm>
#include <cstring>
namespace bdsp_protocol {
using signal_contracts::Status;
namespace {
std::uint32_t fragment_word(std::span<const std::uint8_t> bytes,std::size_t at) noexcept {
    std::uint32_t value=0;
    for(int i=0;i<4;++i)value|=std::uint32_t(bytes[at+i])<<(8*i);
    return value;
}
struct FragmentHeader {std::size_t total,offset,chunk;};
bool fragment_header(std::span<const std::uint8_t> bytes,FragmentHeader& header) noexcept {
    if(bytes.size()<9 || bytes.size()>8+StreamFragmentBytes)return false;
    header={fragment_word(bytes,0),fragment_word(bytes,4),bytes.size()-8};
    return header.total>=StreamHeaderBytes && header.total<=MaxFrameBytes &&
        header.offset<=header.total && header.chunk<=header.total-header.offset;
}
}
CodecResult encode_fragment(std::span<const std::uint8_t> frame,std::size_t offset,std::span<std::uint8_t> out,std::size_t max_chunk_bytes) noexcept {
    if(!max_chunk_bytes || max_chunk_bytes>StreamFragmentBytes || frame.size()<StreamHeaderBytes || frame.size()>MaxFrameBytes || offset>=frame.size())return {Status::invalid_argument};
    const auto chunk=std::min(max_chunk_bytes,frame.size()-offset);
    const auto required=8+chunk;
    if(out.size()<required)return {Status::buffer_too_small,required};
    for(int i=0;i<4;++i){out[i]=std::uint8_t(frame.size()>>(8*i));out[4+i]=std::uint8_t(offset>>(8*i));}
    std::memcpy(out.data()+8,frame.data()+offset,chunk);
    return {Status::ok,required,required};
}
static Status fragment_admission(std::span<const std::uint8_t> fragment,std::size_t capacity,
    std::size_t total,std::size_t received,FragmentHeader& header) noexcept {
    if(!fragment_header(fragment,header))return Status::malformed;
    if(header.total>capacity)return Status::buffer_too_small;
    if(header.offset==0){total=header.total;received=0;}
    if(header.total!=total || header.offset!=received)return Status::malformed;
    return Status::ok;
}
Status FrameAssembler::accept(std::span<const std::uint8_t> fragment,std::span<const std::uint8_t> &complete) noexcept {
    complete={};
    FragmentHeader header{};
    const auto admitted=fragment_admission(fragment,storage_.size(),total_,received_,header);
    if(admitted!=Status::ok){reset();return admitted;}
    if(header.offset==0){reset();total_=header.total;}
    std::memcpy(storage_.data()+header.offset,fragment.data()+8,header.chunk);
    received_+=header.chunk;
    if(received_!=total_)return Status::would_block;
    complete=storage_.first(total_);reset();return Status::ok;
}
}
