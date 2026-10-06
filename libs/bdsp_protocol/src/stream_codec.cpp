#include "bdsp_protocol/stream_codec.h"
#include <bit>
#include <cmath>
#include <cstring>
namespace bdsp_protocol {
using namespace signal_contracts;
namespace {
struct Cursor {
    std::span<const std::uint8_t> bytes; std::size_t position=0;
    std::uint64_t get(int n) noexcept {std::uint64_t v=0;for(int i=0;i<n;++i)v|=std::uint64_t(bytes[position++])<<(i*8);return v;}
};
CodecResult fail(CodecError e) noexcept {
    return {e==CodecError::version?Status::unsupported_version:Status::malformed,0,0,0,0,e};
}
bool metadata_valid(const FrameMetadata &m) noexcept {
    return m.sample_rate<=768000 && m.fft_size<=65536 && m.bands<=4096 && m.history_capacity<=2000 &&
        m.stream_id.size()<=256 && std::isfinite(m.frequency_step) && m.frequency_step>=0 &&
        std::isfinite(m.peak_hz) && std::isfinite(m.magnitude) && (!m.available || (m.sequence && m.sample_rate));
}
bool finite(std::span<const float> v) noexcept {for(float x:v)if(!std::isfinite(x))return false;return true;}
}
const char *error_message(CodecError e) noexcept {
    switch(e){case CodecError::none:return "";case CodecError::size:return "Invalid frame size";
    case CodecError::version:return "Unsupported stream version";case CodecError::flags:return "Invalid flags";
    case CodecError::reserved:return "Reserved fields";case CodecError::dimensions:return "Invalid dimensions/type/length";
    case CodecError::values:return "Invalid frame values";}return "Invalid frame";
}
CodecResult encode(const FrameView &f, std::span<std::uint8_t> out) noexcept {
    const auto &m=f.metadata;const int t=int(f.type);std::size_t columns=0,rows=0,count=0;
    if(t<0 || t>6 || !metadata_valid(m) || !finite(f.left) || !finite(f.right) || !finite(f.values))return fail(CodecError::values);
    if((t!=6 || !m.available) && !f.history.newest_override.empty())return fail(CodecError::values);
    if(!m.available) {
        if(m.sequence || !f.left.empty() || !f.right.empty() || !f.values.empty() || f.history.frames || !f.history.storage.empty())return fail(CodecError::values);
    } else {
        rows=1;
        if(t<=3){columns=f.left.size();if(!columns || columns!=f.right.size())return fail(CodecError::dimensions);}
        else if(t<=5)columns=f.values.size();
        else {
            const auto &h=f.history; columns=h.bands;rows=h.frames;
            if(!h.bands || !h.capacity || h.frames>h.capacity || h.oldest>=h.capacity || h.bands>MaxFrameBytes/4 ||
               h.capacity>MaxFrameBytes/4/h.bands || h.storage.size()!=h.capacity*h.bands)return fail(CodecError::dimensions);
            if(!h.newest_override.empty() && h.newest_override.size()!=h.bands)return fail(CodecError::dimensions);
            for(std::size_t i=0;i<rows;++i)if(!finite(h.row(i)))return fail(CodecError::values);
        }
        if(!columns || !rows || columns>MaxFrameBytes/4 || rows>MaxFrameBytes/4/columns)return fail(CodecError::dimensions);
        count=columns*rows*(t<=3?2:1);
    }
    if(count>(MaxFrameBytes-StreamHeaderBytes-m.stream_id.size())/4)return fail(CodecError::size);
    const auto required=StreamHeaderBytes+m.stream_id.size()+count*4;
    CodecResult result{Status::ok,required,0,count,m.stream_id.size()};
    if(out.size()<required){result.status=Status::buffer_too_small;return result;}
    std::size_t p=0;
    auto put=[&](std::uint64_t n,int bytes){for(int i=0;i<bytes;++i)out[p++]=std::uint8_t(n>>(i*8));};
    put(2,2);put(t,1);put((m.discontinuity?1:0)|(m.bark_weighted?2:0)|(m.available?0:4),1);
    for(auto v:{m.epoch,m.producer_epoch,m.generation,m.sequence,m.source_sequence,m.graph_revision,std::bit_cast<std::uint64_t>(m.timestamp_ns)})put(v,8);
    for(auto v:{m.sample_rate,m.fft_size,m.bands,m.history_capacity})put(v,4);
    for(auto v:{m.frequency_step,m.peak_hz,m.magnitude})put(std::bit_cast<std::uint32_t>(v),4);
    put(columns,4);put(rows,4);put(m.stream_id.size(),4);put(0,4);put(0,8);
    if(!m.stream_id.empty())std::memcpy(out.data()+p,m.stream_id.data(),m.stream_id.size());
    p+=m.stream_id.size();
    auto floats=[&](auto values){for(float x:values)put(std::bit_cast<std::uint32_t>(x),4);};
    if(m.available){if(t<=3){floats(f.left);floats(f.right);}else if(t<=5)floats(f.values);else for(std::size_t i=0;i<rows;++i)floats(f.history.row(i));}
    result.written_bytes=p;return result;
}
CodecResult decode(std::span<const std::uint8_t> bytes,std::span<float> samples,std::span<char> id,FrameView &out) noexcept {
    if(bytes.size()<StreamHeaderBytes || bytes.size()>MaxFrameBytes)return fail(CodecError::size);
    Cursor c{bytes};if(c.get(2)!=2)return fail(CodecError::version);
    FrameView f;f.type=FrameType(c.get(1));const auto flags=c.get(1);if(flags>7)return fail(CodecError::flags);
    auto &m=f.metadata;m.available=!(flags&4);m.discontinuity=flags&1;m.bark_weighted=flags&2;
    m.epoch=c.get(8);m.producer_epoch=c.get(8);m.generation=c.get(8);m.sequence=c.get(8);
    m.source_sequence=c.get(8);m.graph_revision=c.get(8);m.timestamp_ns=std::bit_cast<std::int64_t>(c.get(8));
    m.sample_rate=c.get(4);m.fft_size=c.get(4);m.bands=c.get(4);m.history_capacity=c.get(4);
    m.frequency_step=std::bit_cast<float>(std::uint32_t(c.get(4)));m.peak_hz=std::bit_cast<float>(std::uint32_t(c.get(4)));m.magnitude=std::bit_cast<float>(std::uint32_t(c.get(4)));
    const auto columns=c.get(4),rows=c.get(4),id_size=c.get(4);if(c.get(4) || c.get(8))return fail(CodecError::reserved);
    const int t=int(f.type);
    if(t<0 || t>6 || columns>(MaxFrameBytes-StreamHeaderBytes)/4 || rows>(MaxFrameBytes-StreamHeaderBytes)/4 || id_size>256)return fail(CodecError::dimensions);
    const auto count=columns*rows*(t<=3?2:1);
    if((m.available && (!columns || !rows || (t!=6 && rows!=1))) || (!m.available && (columns || rows || m.sequence)) ||
       count>(MaxFrameBytes-StreamHeaderBytes)/4 || bytes.size()!=StreamHeaderBytes+id_size+count*4)return fail(CodecError::dimensions);
    // Validate the complete payload before touching caller memory.
    m.stream_id={reinterpret_cast<const char *>(bytes.data()+StreamHeaderBytes),std::size_t(id_size)};
    if(!metadata_valid(m))return fail(CodecError::values);
    c.position=StreamHeaderBytes+id_size;
    for(std::size_t i=0;i<count;++i)if(!std::isfinite(std::bit_cast<float>(std::uint32_t(c.get(4)))))return fail(CodecError::values);
    CodecResult result{Status::ok,bytes.size(),0,std::size_t(count),std::size_t(id_size)};
    if(samples.size()<count || id.size()<id_size){result.status=Status::buffer_too_small;return result;}
    if(id_size)std::memcpy(id.data(),bytes.data()+StreamHeaderBytes,id_size);
    m.stream_id={id.data(),std::size_t(id_size)};c.position=StreamHeaderBytes+id_size;
    for(std::size_t i=0;i<count;++i)samples[i]=std::bit_cast<float>(std::uint32_t(c.get(4)));
    if(m.available){if(t<=3){f.left=samples.first(columns);f.right=samples.subspan(columns,columns);}
    else if(t<=5)f.values=samples.first(columns);else f.history={samples.first(count),std::size_t(columns),std::size_t(rows),std::size_t(rows),0};}
    out=f;result.written_bytes=bytes.size();return result;
}
}
