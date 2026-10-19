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
struct StreamShape {std::size_t columns=0,rows=0,count=0;};
CodecResult history_shape(const FrameView& f,StreamShape& shape) noexcept {
    const auto& h=f.history;
    shape.columns=h.bands;
    shape.rows=h.frames;
    if(!h.bands || !h.capacity || h.frames>h.capacity || h.oldest>=h.capacity || h.bands>MaxFrameBytes/4 ||
       h.capacity>MaxFrameBytes/4/h.bands || h.storage.size()!=h.capacity*h.bands)return fail(CodecError::dimensions);
    if(!h.newest_override.empty() && h.newest_override.size()!=h.bands)return fail(CodecError::dimensions);
    for(std::size_t i=0;i<shape.rows;++i)if(!finite(h.row(i)))return fail(CodecError::values);
    return {};
}
CodecResult available_columns(const FrameView& frame,int type,StreamShape& shape) noexcept {
    if(type<=3) {
        shape.columns=frame.left.size();
        if(!shape.columns || shape.columns!=frame.right.size())return fail(CodecError::dimensions);
    }else if(type==8)shape.columns=frame.packet.size()/4;
    else if(type<=5 || type==7)shape.columns=frame.values.size();
    else return history_shape(frame,shape);
    return {};
}
CodecResult available_shape(const FrameView& f,StreamShape& shape) noexcept {
    const int type=int(f.type);
    shape.rows=1;
    if(const auto status=available_columns(f,type,shape);!status)return status;
    if(!shape.columns || !shape.rows || shape.columns>MaxFrameBytes/4 ||
       shape.rows>MaxFrameBytes/4/shape.columns)return fail(CodecError::dimensions);
    shape.count=shape.columns*shape.rows*(type<=3?2:1);
    return {};
}
CodecResult encode_input_status(const FrameView& f) noexcept {
    const auto& m=f.metadata;
    const int type=int(f.type);
    if(!frame_descriptor(f.type) || !metadata_valid(m) || !finite(f.left) || !finite(f.right) || !finite(f.values))
        return fail(CodecError::values);
    if(validate(f)!=(m.available?Status::ok:Status::unavailable))return fail(CodecError::values);
    if(type!=8 && !f.packet.empty())return fail(CodecError::values);
    if((type!=6 || !m.available) && !f.history.newest_override.empty())return fail(CodecError::values);
    if(!m.available && (m.sequence || !f.left.empty() || !f.right.empty() || !f.values.empty() ||
                        f.history.frames || !f.history.storage.empty()))return fail(CodecError::values);
    return {};
}
CodecResult encode_shape(const FrameView& f,StreamShape& shape) noexcept {
    const auto& m=f.metadata;
    if(auto status=encode_input_status(f);!status)return status;
    if(m.available)if(auto status=available_shape(f,shape);!status)return status;
    if(!compatible_geometry(f.type,m,shape.columns,shape.rows))return fail(CodecError::dimensions);
    if(shape.count>(MaxFrameBytes-StreamHeaderBytes-m.stream_id.size())/4)return fail(CodecError::size);
    return {};
}
struct StreamWriter {
    std::span<std::uint8_t> out;
    std::size_t position=0;
    void put(std::uint64_t value,int bytes) noexcept {
        for(int i=0;i<bytes;++i)out[position++]=std::uint8_t(value>>(i*8));
    }
    void floats(std::span<const float> values) noexcept {
        for(float value:values)put(std::bit_cast<std::uint32_t>(value),4);
    }
};
void write_stamp(StreamWriter& writer,const FrameMetadata& m) noexcept {
    for(auto value:{m.epoch,m.producer_epoch,m.generation,m.sequence,m.source_sequence,m.graph_revision,
                    std::bit_cast<std::uint64_t>(m.timestamp_ns)})writer.put(value,8);
}
void write_metadata(StreamWriter& writer,const FrameMetadata& m) noexcept {
    for(auto value:{m.sample_rate,m.fft_size,m.bands,m.history_capacity})writer.put(value,4);
    for(auto value:{m.frequency_step,m.peak_hz,m.magnitude})writer.put(std::bit_cast<std::uint32_t>(value),4);
}
void write_header(StreamWriter& writer,const FrameView& f,const StreamShape& shape) noexcept {
    const auto& m=f.metadata;
    const int type=int(f.type);
    writer.put(type>=7?3:2,2);
    writer.put(type,1);
    writer.put((m.discontinuity?1:0)|(m.bark_weighted?2:0)|(m.available?0:4),1);
    write_stamp(writer,m);
    write_metadata(writer,m);
    writer.put(shape.columns,4);
    writer.put(shape.rows,4);
    writer.put(m.stream_id.size(),4);
    writer.put(0,4);
    writer.put(0,8);
    if(!m.stream_id.empty())std::memcpy(writer.out.data()+writer.position,m.stream_id.data(),m.stream_id.size());
    writer.position+=m.stream_id.size();
}
void write_payload(StreamWriter& writer,const FrameView& f,const StreamShape& shape) noexcept {
    if(!f.metadata.available)return;
    const int type=int(f.type);
    if(type==8) {
        std::memcpy(writer.out.data()+writer.position,f.packet.data(),f.packet.size());
        writer.position+=f.packet.size();
    } else if(type<=3) {
        writer.floats(f.left);
        writer.floats(f.right);
    } else if(type<=5 || type==7)writer.floats(f.values);
    else for(std::size_t i=0;i<shape.rows;++i)writer.floats(f.history.row(i));
}
struct DecodeShape {std::uint64_t version=0,columns=0,rows=0,id_size=0,count=0;};
void read_stamp(Cursor& cursor,FrameMetadata& m) noexcept {
    m.epoch=cursor.get(8);m.producer_epoch=cursor.get(8);m.generation=cursor.get(8);m.sequence=cursor.get(8);
    m.source_sequence=cursor.get(8);m.graph_revision=cursor.get(8);
    m.timestamp_ns=std::bit_cast<std::int64_t>(cursor.get(8));
}
void read_metadata(Cursor& cursor,FrameMetadata& m) noexcept {
    m.sample_rate=cursor.get(4);m.fft_size=cursor.get(4);m.bands=cursor.get(4);m.history_capacity=cursor.get(4);
    m.frequency_step=std::bit_cast<float>(std::uint32_t(cursor.get(4)));
    m.peak_hz=std::bit_cast<float>(std::uint32_t(cursor.get(4)));
    m.magnitude=std::bit_cast<float>(std::uint32_t(cursor.get(4)));
}
void read_header_metadata(Cursor& cursor,FrameMetadata& metadata,std::uint64_t flags) noexcept {
    metadata.available=!(flags&4);
    metadata.discontinuity=flags&1;
    metadata.bark_weighted=flags&2;
    read_stamp(cursor,metadata);
    read_metadata(cursor,metadata);
}
CodecResult read_header(Cursor& cursor,FrameView& frame,DecodeShape& shape) noexcept {
    shape.version=cursor.get(2);
    if(shape.version!=2 && shape.version!=3)return fail(CodecError::version);
    frame.type=FrameType(cursor.get(1));
    const auto flags=cursor.get(1);
    if(flags>7)return fail(CodecError::flags);
    read_header_metadata(cursor,frame.metadata,flags);
    shape.columns=cursor.get(4);
    shape.rows=cursor.get(4);
    shape.id_size=cursor.get(4);
    if(cursor.get(4) || cursor.get(8))return fail(CodecError::reserved);
    return {};
}
bool special_type_geometry(const FrameView& frame,const DecodeShape& shape) noexcept {
    const auto& m=frame.metadata;
    const int type=int(frame.type);
    if(type==7 && ((m.available && shape.columns!=1) || m.fft_size || m.bands || m.history_capacity ||
                   m.frequency_step || m.bark_weighted))return false;
    if(type==8 && ((m.available && shape.columns!=11) || m.fft_size || m.bands || m.history_capacity ||
                   m.frequency_step || m.bark_weighted))return false;
    return true;
}
bool payload_dimensions_valid(const FrameView& frame,const DecodeShape& shape,std::size_t size) noexcept {
    const auto& m=frame.metadata;
    const int type=int(frame.type);
    if(m.available && (!shape.columns || !shape.rows || (type!=6 && shape.rows!=1)))return false;
    if(!m.available && (shape.columns || shape.rows || m.sequence))return false;
    return shape.count<=(MaxFrameBytes-StreamHeaderBytes)/4 &&
        size==StreamHeaderBytes+shape.id_size+shape.count*4;
}
CodecResult validate_decoded_shape(const FrameView& frame,DecodeShape& shape,std::size_t size) noexcept {
    const auto& m=frame.metadata;
    const int type=int(frame.type);
    if((shape.version==2 && type>=7) || (shape.version==3 && type<7))return fail(CodecError::version);
    if(!frame_descriptor(frame.type) || shape.columns>(MaxFrameBytes-StreamHeaderBytes)/4 ||
       shape.rows>(MaxFrameBytes-StreamHeaderBytes)/4 || shape.id_size>256)return fail(CodecError::dimensions);
    if(!special_type_geometry(frame,shape))return fail(CodecError::dimensions);
    shape.count=shape.columns*shape.rows*(type<=3?2:1);
    if(!payload_dimensions_valid(frame,shape,size))return fail(CodecError::dimensions);
    if(!compatible_geometry(frame.type,m,std::size_t(shape.columns),std::size_t(shape.rows)))
        return fail(CodecError::dimensions);
    return {};
}
CodecResult validate_payload(Cursor& cursor,FrameView& frame,const DecodeShape& shape) noexcept {
    auto& m=frame.metadata;
    m.stream_id={reinterpret_cast<const char *>(cursor.bytes.data()+StreamHeaderBytes),std::size_t(shape.id_size)};
    if(!metadata_valid(m))return fail(CodecError::values);
    cursor.position=StreamHeaderBytes+shape.id_size;
    if(int(frame.type)==8 && m.available) {
        frame.packet=cursor.bytes.subspan(cursor.position,44);
        if(validate(frame)!=Status::ok)return fail(CodecError::values);
    } else for(std::size_t i=0;i<shape.count;++i)
        if(!std::isfinite(std::bit_cast<float>(std::uint32_t(cursor.get(4)))))return fail(CodecError::values);
    return {};
}
void assign_payload_views(FrameView& frame,std::span<float> samples,const DecodeShape& shape) noexcept {
    if(!frame.metadata.available)return;
    const int type=int(frame.type);
    if(type<=3) {
        frame.left=samples.first(shape.columns);
        frame.right=samples.subspan(shape.columns,shape.columns);
    } else if(type==8) {}
    else if(type<=5 || type==7)frame.values=samples.first(shape.columns);
    else frame.history={samples.first(shape.count),std::size_t(shape.columns),std::size_t(shape.rows),
                        std::size_t(shape.rows),0};
}
void copy_payload(Cursor& cursor,FrameView& frame,const DecodeShape& shape,std::span<float> samples,
                  std::span<char> id,std::span<std::uint8_t> packet) noexcept {
    if(shape.id_size)std::memcpy(id.data(),cursor.bytes.data()+StreamHeaderBytes,shape.id_size);
    frame.metadata.stream_id={id.data(),std::size_t(shape.id_size)};
    cursor.position=StreamHeaderBytes+shape.id_size;
    if(int(frame.type)==8 && frame.metadata.available) {
        std::memcpy(packet.data(),cursor.bytes.data()+cursor.position,44);
        frame.packet=packet.first(44);
    } else for(std::size_t i=0;i<shape.count;++i)
        samples[i]=std::bit_cast<float>(std::uint32_t(cursor.get(4)));
    assign_payload_views(frame,samples,shape);
}
}
bool compatible_geometry(FrameType type,const FrameMetadata &m,std::size_t columns,std::size_t rows) noexcept {
    if(!m.available)return !columns && !rows;
    if(type==FrameType::History)return rows<=2000 && (!m.bands || m.bands==columns) && (!m.history_capacity || rows<=m.history_capacity);
    if(rows!=1)return false;
    if(type==FrameType::Window || type==FrameType::Windowed)return !m.fft_size || columns==m.fft_size;
    if(type==FrameType::StereoSpectrum || type==FrameType::Spectrum)return !m.fft_size || columns==m.fft_size/2;
    if(type==FrameType::Mel)return !m.bands || columns==m.bands;
    return true;
}
const char *error_message(CodecError e) noexcept {
    switch(e){case CodecError::none:return "";case CodecError::size:return "Invalid frame size";
    case CodecError::version:return "Unsupported stream version";case CodecError::flags:return "Invalid flags";
    case CodecError::reserved:return "Reserved fields";case CodecError::dimensions:return "Invalid dimensions/type/length";
    case CodecError::values:return "Invalid frame values";}return "Invalid frame";
}
CodecResult encode(const FrameView &f, std::span<std::uint8_t> out) noexcept {
    StreamShape shape;
    if(auto status=encode_shape(f,shape);!status)return status;
    const auto required=StreamHeaderBytes+f.metadata.stream_id.size()+shape.count*4;
    CodecResult result{Status::ok,required,0,shape.count,f.metadata.stream_id.size()};
    if(out.size()<required){result.status=Status::buffer_too_small;return result;}
    StreamWriter writer{out};
    write_header(writer,f,shape);
    write_payload(writer,f,shape);
    result.written_bytes=writer.position;
    return result;
}
static CodecResult decoded_capacity(const FrameView& frame,const DecodeShape& shape,
    std::size_t wire_bytes,std::span<float> samples,std::span<char> id,
    std::span<std::uint8_t> packet) noexcept {
    CodecResult result{Status::ok,wire_bytes,0,int(frame.type)==8?0:std::size_t(shape.count),
                       std::size_t(shape.id_size)};
    result.required_packet_bytes=int(frame.type)==8 && frame.metadata.available?44:0;
    if(samples.size()<result.required_samples || id.size()<shape.id_size ||
       packet.size()<result.required_packet_bytes)result.status=Status::buffer_too_small;
    return result;
}
static CodecResult validate_encoded_frame(Cursor& cursor,FrameView& frame,
                                          DecodeShape& shape,std::size_t wire_bytes) noexcept {
    const auto header=read_header(cursor,frame,shape);
    if(!header)return header;
    const auto dimensions=validate_decoded_shape(frame,shape,wire_bytes);
    if(!dimensions)return dimensions;
    return validate_payload(cursor,frame,shape);
}
CodecResult decode(std::span<const std::uint8_t> bytes,std::span<float> samples,std::span<char> id,
                   std::span<std::uint8_t> packet,FrameView &out) noexcept {
    if(bytes.size()<StreamHeaderBytes || bytes.size()>MaxFrameBytes)return fail(CodecError::size);
    Cursor cursor{bytes};
    FrameView frame;
    DecodeShape shape;
    const auto valid=validate_encoded_frame(cursor,frame,shape,bytes.size());
    if(!valid)return valid;
    auto result=decoded_capacity(frame,shape,bytes.size(),samples,id,packet);
    if(!result)return result;
    copy_payload(cursor,frame,shape,samples,id,packet);
    out=frame;
    result.written_bytes=bytes.size();
    return result;
}
CodecResult decode(std::span<const std::uint8_t> bytes,std::span<float> samples,std::span<char> id,FrameView &out) noexcept {
    // Explicit old float-backed packet storage adapter, preserved for SDK callers.
    auto packet=std::span(reinterpret_cast<std::uint8_t *>(samples.data()),samples.size_bytes());
    auto result=decode(bytes,samples,id,packet,out);
    if(result.required_packet_bytes)result.required_samples=result.required_packet_bytes/sizeof(float);
    return result;
}
}
