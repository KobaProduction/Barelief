#include "bdsp_protocol/stream_fragments.h"
#include <algorithm>
#include <cstring>
namespace bdsp_protocol {
using signal_contracts::Status;
CodecResult encode_fragment(std::span<const std::uint8_t> frame,std::size_t offset,std::span<std::uint8_t> out,std::size_t max_chunk_bytes) noexcept {
    if(!max_chunk_bytes || max_chunk_bytes>StreamFragmentBytes || frame.size()<StreamHeaderBytes || frame.size()>MaxFrameBytes || offset>=frame.size())return {Status::invalid_argument};
    const auto chunk=std::min(max_chunk_bytes,frame.size()-offset);
    const auto required=8+chunk;
    if(out.size()<required)return {Status::buffer_too_small,required};
    for(int i=0;i<4;++i){out[i]=std::uint8_t(frame.size()>>(8*i));out[4+i]=std::uint8_t(offset>>(8*i));}
    std::memcpy(out.data()+8,frame.data()+offset,chunk);
    return {Status::ok,required,required};
}
Status FrameAssembler::accept(std::span<const std::uint8_t> fragment,std::span<const std::uint8_t> &complete) noexcept {
    complete={};
    auto fail=[&](Status status){reset();return status;};
    if(fragment.size()<9 || fragment.size()>8+StreamFragmentBytes)return fail(Status::malformed);
    auto get=[&](std::size_t p){std::uint32_t v=0;for(int i=0;i<4;++i)v|=std::uint32_t(fragment[p+i])<<(8*i);return v;};
    const std::size_t total=get(0),offset=get(4),chunk=fragment.size()-8;
    if(total<StreamHeaderBytes || total>MaxFrameBytes || offset>total || chunk>total-offset)return fail(Status::malformed);
    if(total>storage_.size())return fail(Status::buffer_too_small);
    if(offset==0){reset();total_=total;}
    if(total!=total_ || offset!=received_)return fail(Status::malformed);
    std::memcpy(storage_.data()+offset,fragment.data()+8,chunk);received_+=chunk;
    if(received_!=total_)return Status::would_block;
    complete=storage_.first(total_);reset();return Status::ok;
}
}
