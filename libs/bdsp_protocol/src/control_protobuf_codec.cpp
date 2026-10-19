#include <bdsp_protocol/control_codec.h>
#include "control.pb.h"
#include <pb_common.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <iterator>

#ifdef PB_ENABLE_MALLOC
#error Control codecs require the caller-buffer nanopb runtime without PB_ENABLE_MALLOC
#endif
#ifdef PB_WITHOUT_64BIT
#error Control schema requires lossless uint64 fields
#endif

namespace bdsp_protocol {
namespace {
using namespace detail;
const pb_msgdesc_t* descriptor(ControlSchema schema) noexcept {
    static const pb_msgdesc_t* const descriptors[]{
        &bdsp_control_v1_SessionIdentity_msg, &bdsp_control_v1_NoParameters_msg,
        &bdsp_control_v1_AudioSource_msg, &bdsp_control_v1_Merge_msg,
        &bdsp_control_v1_Gain_msg, &bdsp_control_v1_Mel_msg,
        &bdsp_control_v1_History_msg, &bdsp_control_v1_Window_msg,
        &bdsp_control_v1_WindowFunction_msg, &bdsp_control_v1_TestSignal_msg,
        &bdsp_control_v1_Network_msg, &bdsp_control_v1_Routing_msg,
        &bdsp_control_v1_Reduction_msg, &bdsp_control_v1_Parameters_msg,
        &bdsp_control_v1_Node_msg, &bdsp_control_v1_Edge_msg,
        &bdsp_control_v1_ProcessingGraph_msg, &bdsp_control_v1_Command_msg,
        &bdsp_control_v1_Diagnostic_msg, &bdsp_control_v1_Memory_msg,
        &bdsp_control_v1_State_msg, &bdsp_control_v1_Capabilities_msg,
        &bdsp_control_v1_Reply_msg, &bdsp_control_v1_ControlMessage_msg
    };
    const auto index=static_cast<unsigned>(schema);
    return index<std::size(descriptors) ? descriptors[index] : nullptr;
}
struct CallbackRecord {
    std::string_view text;
    ControlSchema schema{};
    const void* data=nullptr;
    std::size_t count=0, depth=0;
    ControlElementVisit visit=nullptr;
    ControlArchiveState* state=nullptr;
};
struct CallbackStorage {
    std::array<CallbackRecord,32> records{};
    std::size_t count=0;
    CallbackRecord* acquire(ControlArchiveState& state) noexcept {
        if(count==records.size()) {state.fail(ControlCodecStatus::codec_failure);return nullptr;}
        return &records[count++];
    }
};
bool write_callback(pb_ostream_t*,const pb_field_t*,void* const*) noexcept;
class ProtobufWriter final : public ControlArchive {
    const pb_msgdesc_t* descriptor_;
    void* message_;
    CallbackStorage& callbacks_;
    std::size_t depth_;
    pb_ostream_t* scalar_stream_=nullptr;
    bool find(unsigned tag,pb_field_iter_t& field) noexcept {
        if(depth_>state.limits.max_depth)return state.fail(ControlCodecStatus::limit_exceeded);
        if(!pb_field_iter_begin(&field,descriptor_,message_) || !pb_field_iter_find(&field,tag))
            return state.fail(ControlCodecStatus::codec_failure);
        if(PB_HTYPE(field.type)==PB_HTYPE_OPTIONAL && field.pSize)
            *static_cast<bool*>(field.pSize)=true;
        return true;
    }
    bool callback(unsigned tag,CallbackRecord*& record) noexcept {
        pb_field_iter_t field;
        if(!find(tag,field) || PB_ATYPE(field.type)!=PB_ATYPE_CALLBACK)return state.fail(ControlCodecStatus::codec_failure);
        record=callbacks_.acquire(state);
        if(!record)return false;
        record->state=&state;
        auto* callback=static_cast<pb_callback_t*>(field.pData);
        callback->funcs.encode=write_callback;callback->arg=record;
        return true;
    }
public:
    ProtobufWriter(ControlArchiveState& state,const pb_msgdesc_t* descriptor,void* message,
        CallbackStorage& callbacks,std::size_t depth,pb_ostream_t* scalar_stream=nullptr) noexcept
        : ControlArchive(state),descriptor_(descriptor),message_(message),callbacks_(callbacks),depth_(depth),scalar_stream_(scalar_stream) {}
    bool uint_field(unsigned tag,std::uint64_t& value) noexcept override {
        if(scalar_stream_)return pb_encode_varint(scalar_stream_,value) || state.fail(ControlCodecStatus::codec_failure);
        pb_field_iter_t field;
        if(!find(tag,field))return false;
        if(field.data_size==sizeof(std::uint32_t)) {
            if(value>UINT32_MAX)return state.fail();
            const auto number=static_cast<std::uint32_t>(value);std::memcpy(field.pData,&number,sizeof(number));
        } else if(field.data_size==sizeof(std::uint64_t))std::memcpy(field.pData,&value,sizeof(value));
        else return state.fail(ControlCodecStatus::codec_failure);
        return true;
    }
    bool optional_uint_field(unsigned tag,std::uint64_t& value,bool& present) noexcept override {
        return !present || uint_field(tag,value);
    }
    bool int_field(unsigned tag,std::int32_t& value) noexcept override {
        pb_field_iter_t field;
        if(!find(tag,field) || field.data_size!=sizeof(value))return state.fail(ControlCodecStatus::codec_failure);
        std::memcpy(field.pData,&value,sizeof(value));return true;
    }
    bool double_field(unsigned tag,double& value) noexcept override {
        pb_field_iter_t field;
        if(!find(tag,field) || field.data_size!=sizeof(value))return state.fail(ControlCodecStatus::codec_failure);
        std::memcpy(field.pData,&value,sizeof(value));return true;
    }
    bool bool_field(unsigned tag,bool& value) noexcept override {
        pb_field_iter_t field;
        if(!find(tag,field) || field.data_size!=sizeof(value))return state.fail(ControlCodecStatus::codec_failure);
        std::memcpy(field.pData,&value,sizeof(value));return true;
    }
    bool text_field(unsigned tag,std::string_view& value,std::size_t) noexcept override {
        if(scalar_stream_)return pb_encode_string(scalar_stream_,
            reinterpret_cast<const pb_byte_t*>(value.empty()?"":value.data()),value.size()) || state.fail(ControlCodecStatus::codec_failure);
        CallbackRecord* record=nullptr;
        if(!callback(tag,record))return false;
        record->text=value;return true;
    }
    bool object_field(unsigned tag,ControlSchema schema,void* value,ControlVisit visit,bool* present) noexcept override {
        if(present && !*present)return true;
        if(depth_+1>state.limits.max_depth)return state.fail(ControlCodecStatus::limit_exceeded);
        pb_field_iter_t field;
        if(!find(tag,field) || field.submsg_desc!=descriptor(schema))return state.fail(ControlCodecStatus::codec_failure);
        ProtobufWriter child(state,field.submsg_desc,field.pData,callbacks_,depth_+1);
        return visit(child,value);
    }
    bool sequence_field(unsigned tag,ControlSchema schema,const void* data,std::size_t& count,ControlElementVisit visit) noexcept override {
        if(depth_+1>state.limits.max_depth)return state.fail(ControlCodecStatus::limit_exceeded);
        CallbackRecord* record=nullptr;
        if(!callback(tag,record))return false;
        record->schema=schema;record->data=data;record->count=count;record->visit=visit;record->depth=depth_;
        return true;
    }
};
template <class T>
bool write_message_element(pb_ostream_t* stream,CallbackRecord& record,std::size_t index,
                           const pb_msgdesc_t* descriptor) noexcept {
    T value{};
    CallbackStorage callbacks;
    ProtobufWriter writer(*record.state,descriptor,&value,callbacks,record.depth+2);
    return record.visit(writer,record.data,index) && pb_encode_submessage(stream,descriptor,&value);
}
bool write_element(pb_ostream_t* stream,const pb_field_t* field,CallbackRecord& record,std::size_t index) noexcept {
    if(!pb_encode_tag_for_field(stream,field))return false;
    if(record.schema==ControlSchema::TextElement || record.schema==ControlSchema::TypeElement) {
        CallbackStorage callbacks;
        ProtobufWriter writer(*record.state,nullptr,nullptr,callbacks,record.depth+1,stream);
        return record.visit(writer,record.data,index);
    }
    if(record.schema==ControlSchema::Node)
        return write_message_element<bdsp_control_v1_Node>(stream,record,index,&bdsp_control_v1_Node_msg);
    if(record.schema==ControlSchema::Edge)
        return write_message_element<bdsp_control_v1_Edge>(stream,record,index,&bdsp_control_v1_Edge_msg);
    return record.state->fail(ControlCodecStatus::codec_failure);
}
bool write_callback(pb_ostream_t* stream,const pb_field_t* field,void* const* argument) noexcept {
    auto& record=*static_cast<CallbackRecord*>(*argument);
    if(!record.visit)
        return pb_encode_tag_for_field(stream,field) && pb_encode_string(stream,
            reinterpret_cast<const pb_byte_t*>(record.text.empty()?"":record.text.data()),record.text.size());
    for(std::size_t i=0;i<record.count;++i)
        if(!write_element(stream,field,record,i))return false;
    return true;
}
pb_wire_type_t wire_type(pb_type_t type) noexcept {
    switch(PB_LTYPE(type)) {
    case PB_LTYPE_BOOL: case PB_LTYPE_VARINT: case PB_LTYPE_UVARINT: case PB_LTYPE_SVARINT: return PB_WT_VARINT;
    case PB_LTYPE_FIXED64: return PB_WT_64BIT;
    case PB_LTYPE_FIXED32: return PB_WT_32BIT;
    default: return PB_WT_STRING;
    }
}
// A strict descriptor-aware pass is necessary: nanopb intentionally permits
// repeated singular tags and unknown fields. It uses nanopb primitives, not a
// second hand-coded protobuf byte format. Generated descriptors define types.
class ProtobufReader final : public ControlArchive {
    std::span<const std::uint8_t> source_;
    std::array<std::span<const std::uint8_t>,21> values_{};
    std::uint32_t present_=0,used_=0;
    ControlSchema schema_;
    std::size_t depth_;
    bool take(unsigned tag,pb_istream_t& stream) noexcept {
        const auto bit=std::uint32_t{1}<<tag;
        if(!(present_&bit))return state.fail();
        used_|=bit;
        stream=pb_istream_from_buffer(values_[tag].data(),values_[tag].size());return true;
    }
    bool subspan(unsigned tag,std::span<const std::uint8_t>& value) noexcept {
        pb_istream_t stream,substream;
        if(!take(tag,stream) || !pb_make_string_substream(&stream,&substream))return state.fail();
        value={static_cast<const std::uint8_t*>(substream.state),substream.bytes_left};
        return true;
    }
    bool verify_field(std::uint32_t tag,pb_wire_type_t wire,const pb_msgdesc_t* desc) noexcept {
        pb_field_iter_t field;
        if(tag>20 || !pb_field_iter_begin(&field,desc,nullptr) || !pb_field_iter_find(&field,tag))
            return state.fail(ControlCodecStatus::unsupported_field);
        if(wire!=wire_type(field.type))return state.fail();
        const auto bit=std::uint32_t{1}<<tag;
        if((present_&bit) && PB_HTYPE(field.type)!=PB_HTYPE_REPEATED)return state.fail();
        return true;
    }
    bool read_field(pb_istream_t& stream,const pb_msgdesc_t* desc) noexcept {
        pb_wire_type_t wire;std::uint32_t tag=0;bool eof=false;
        if(!pb_decode_tag(&stream,&wire,&tag,&eof) || tag==0)return state.fail();
        if(!verify_field(tag,wire,desc))return false;
        const auto bit=std::uint32_t{1}<<tag;
        const auto* start=static_cast<const std::uint8_t*>(stream.state);
        const auto remaining=stream.bytes_left;
        if(!pb_skip_field(&stream,wire))return state.fail();
        values_[tag]={start,remaining-stream.bytes_left};
        present_|=bit;
        return true;
    }
    bool read_sequence_element(std::span<const std::uint8_t> element,ControlSchema schema,
                               const void* data,std::size_t index,ControlElementVisit visit) noexcept {
        const bool scalar=schema==ControlSchema::TextElement || schema==ControlSchema::TypeElement;
        if(!scalar) {
            auto value_stream=pb_istream_from_buffer(element.data(),element.size());
            pb_istream_t substream;
            if(!pb_make_string_substream(&value_stream,&substream))return state.fail();
            element={static_cast<const std::uint8_t*>(substream.state),substream.bytes_left};
        }
        ProtobufReader child(state,element,schema,depth_+(scalar?1:2));
        return state.error==ControlCodecStatus::ok && visit(child,data,index) && child.finish();
    }
    std::size_t sequence_limit(ControlSchema schema) const noexcept {
        if(schema==ControlSchema::Node)return state.limits.max_nodes;
        if(schema==ControlSchema::Edge)return state.limits.max_edges;
        if(schema==ControlSchema::TextElement)return state.limits.max_capability_kinds;
        return state.limits.max_capability_types;
    }
    bool sequence_entry(pb_istream_t& stream,unsigned tag,ControlSchema schema,
                        const void* data,std::size_t& count,std::size_t maximum,
                        ControlElementVisit visit) noexcept {
        pb_wire_type_t wire;std::uint32_t field_tag=0;bool eof=false;
        if(!pb_decode_tag(&stream,&wire,&field_tag,&eof))return state.fail();
        const auto* start=static_cast<const std::uint8_t*>(stream.state);
        const auto remaining=stream.bytes_left;
        if(!pb_skip_field(&stream,wire))return state.fail();
        if(field_tag!=tag)return true;
        if(count>=maximum)return state.fail(ControlCodecStatus::limit_exceeded);
        std::span<const std::uint8_t> element{start,remaining-stream.bytes_left};
        if(!read_sequence_element(element,schema,data,count,visit))return false;
        ++count;
        return true;
    }
public:
    ProtobufReader(ControlArchiveState& state,std::span<const std::uint8_t> source,
        ControlSchema schema,std::size_t depth) noexcept : ControlArchive(state),source_(source),schema_(schema),depth_(depth) {
        if(depth>state.limits.max_depth){state.fail(ControlCodecStatus::limit_exceeded);return;}
        if(schema==ControlSchema::TextElement || schema==ControlSchema::TypeElement){values_[0]=source;present_=1;return;}
        auto stream=pb_istream_from_buffer(source.data(),source.size());
        const auto* desc=descriptor(schema);
        while(stream.bytes_left) if(!read_field(stream,desc)) return;
    }
    bool finish() noexcept { return state.error==ControlCodecStatus::ok && (present_==used_ || state.fail()); }
    bool uint_field(unsigned tag,std::uint64_t& value) noexcept override {
        pb_istream_t stream;
        return take(tag,stream) && ((pb_decode_varint(&stream,&value) && stream.bytes_left==0) || state.fail());
    }
    bool optional_uint_field(unsigned tag,std::uint64_t& value,bool& present) noexcept override {
        present=(present_&(std::uint32_t{1}<<tag))!=0;
        return !present || uint_field(tag,value);
    }
    bool int_field(unsigned tag,std::int32_t& value) noexcept override {
        pb_istream_t stream;std::int64_t number=0;
        if(!take(tag,stream))return false;
        if(!pb_decode_svarint(&stream,&number) || stream.bytes_left!=0 ||
            number<INT32_MIN || number>INT32_MAX)return state.fail();
        value=static_cast<std::int32_t>(number);return true;
    }
    bool double_field(unsigned tag,double& value) noexcept override {
        pb_istream_t stream;
        return take(tag,stream) && ((pb_decode_fixed64(&stream,&value) && stream.bytes_left==0 && std::isfinite(value)) || state.fail());
    }
    bool bool_field(unsigned tag,bool& value) noexcept override {
        std::uint64_t integer=0;
        if(!uint_field(tag,integer) || integer>1)return state.fail();
        value=integer!=0;return true;
    }
    bool text_field(unsigned tag,std::string_view& value,std::size_t maximum) noexcept override {
        std::span<const std::uint8_t> bytes;
        if(!subspan(tag,bytes))return false;
        value={reinterpret_cast<const char*>(bytes.data()),bytes.size()};return state.store_text(value,maximum);
    }
    bool object_field(unsigned tag,ControlSchema schema,void* value,ControlVisit visit,bool* present) noexcept override {
        if(present){*present=(present_&(std::uint32_t{1}<<tag))!=0;if(!*present)return true;}
        std::span<const std::uint8_t> bytes;
        if(!subspan(tag,bytes))return false;
        ProtobufReader child(state,bytes,schema,depth_+1);
        return state.error==ControlCodecStatus::ok && visit(child,value) && child.finish();
    }
    bool sequence_field(unsigned tag,ControlSchema schema,const void* data,std::size_t& count,ControlElementVisit visit) noexcept override {
        if(depth_+1>state.limits.max_depth)return state.fail(ControlCodecStatus::limit_exceeded);
        used_|=present_&(std::uint32_t{1}<<tag);
        count=0;
        const auto maximum=sequence_limit(schema);
        auto stream=pb_istream_from_buffer(source_.data(),source_.size());
        while(stream.bytes_left)
            if(!sequence_entry(stream,tag,schema,data,count,maximum,visit))return false;
        return true;
    }
};
bool read_protobuf(std::span<const std::uint8_t> source,ControlArchiveState& state,WireControlMessage& message) noexcept {
    ProtobufReader reader(state,source,ControlSchema::ControlMessage,1);
    return state.error==ControlCodecStatus::ok && visit_control_message(reader,&message) && reader.finish();
}
bool map_protobuf(ControlArchiveState& state,WireControlMessage& message,
                  bdsp_control_v1_ControlMessage& generated,CallbackStorage& callbacks) noexcept {
    ProtobufWriter mapper(state,&bdsp_control_v1_ControlMessage_msg,&generated,callbacks,1);
    return visit_control_message(mapper,&message);
}
bool measure_protobuf(bdsp_control_v1_ControlMessage& generated,std::size_t& size) noexcept {
    pb_ostream_t measure=PB_OSTREAM_SIZING;
    if(!pb_encode(&measure,&bdsp_control_v1_ControlMessage_msg,&generated)) return false;
    size=measure.bytes_written;
    return true;
}
bool emit_protobuf(bdsp_control_v1_ControlMessage& generated,
                   std::span<std::uint8_t> output,std::span<std::uint8_t> scratch,std::size_t size) noexcept {
    auto stream=pb_ostream_from_buffer(scratch.data(),scratch.size());
    if(!pb_encode(&stream,&bdsp_control_v1_ControlMessage_msg,&generated) || stream.bytes_written!=size) return false;
    std::memcpy(output.data(),scratch.data(),size);
    return true;
}
bool generated_protobuf_valid(std::span<const std::uint8_t> source) noexcept {
    bdsp_control_v1_ControlMessage generated{};
    auto stream=pb_istream_from_buffer(source.data(),source.size());
    return pb_decode(&stream,&bdsp_control_v1_ControlMessage_msg,&generated) && stream.bytes_left==0;
}
bool preflight_protobuf(std::span<const std::uint8_t> source,ControlDecodeArena arena,
                        ControlArchiveState& state,WireControlMessage& message,ControlCodecResult& result) noexcept {
    if(source.size()>state.limits.max_wire_bytes){result.status=ControlCodecStatus::limit_exceeded;return false;}
    if(!read_protobuf(source,state,message)){result.status=state.error;return false;}
    if(!generated_protobuf_valid(source)){result.status=ControlCodecStatus::malformed;return false;}
    result.required_arena=state.used;
    if(!control_arena_fits(state.used,arena)){result.status=ControlCodecStatus::buffer_too_small;return false;}
    return true;
}
bool materialize_protobuf(std::span<const std::uint8_t> source,ControlDecodeArena arena,
                          ControlArchiveState& state,WireControlMessage& message) noexcept {
    state.arena=arena;
    state.used={};
    state.materializing=true;
    message={};
    return read_protobuf(source,state,message);
}
ControlCodecResult protobuf_capacity(std::size_t size,std::span<std::uint8_t> output,
                                      std::span<std::uint8_t> scratch,const ControlCodecLimits& limits) noexcept {
    ControlCodecResult result;
    if(size>limits.max_wire_bytes){result.status=ControlCodecStatus::limit_exceeded;return result;}
    result.required_wire_bytes=size;
    result.required_scratch_bytes=size;
    if(output.size()<size || scratch.size()<size)result.status=ControlCodecStatus::buffer_too_small;
    return result;
}
ControlCodecResult encode_validated_protobuf(const WireControlMessage& source,
    std::span<std::uint8_t> output,std::span<std::uint8_t> scratch,
    const ControlCodecLimits& limits) noexcept {
    WireControlMessage message=source;
    bdsp_control_v1_ControlMessage generated{};
    ControlArchiveState state{limits,{},{},ControlCodecStatus::ok,false,false};
    CallbackStorage callbacks;
    if(!map_protobuf(state,message,generated,callbacks))return {state.error};
    std::size_t size=0;
    if(!measure_protobuf(generated,size))
        return {state.error==ControlCodecStatus::ok?ControlCodecStatus::codec_failure:state.error};
    auto result=protobuf_capacity(size,output,scratch,limits);
    if(!result)return result;
    if(!emit_protobuf(generated,output,scratch,size))
        return {state.error==ControlCodecStatus::ok?ControlCodecStatus::codec_failure:state.error};
    result.produced=size;
    return result;
}
}
ControlCodecResult ProtobufControlCodec::encode(const WireControlMessage& source,std::span<std::uint8_t> output,
    std::span<std::uint8_t> scratch,const ControlCodecLimits& limits) const noexcept {
    auto result=validate_control_message(source,limits);
    if(!result)return result;
    return encode_validated_protobuf(source,output,scratch,limits);
}
ControlCodecResult ProtobufControlCodec::decode(std::span<const std::uint8_t> source,ControlDecodeArena arena,
    WireControlMessage& output,const ControlCodecLimits& limits) const noexcept {
    ControlCodecResult result;
    ControlArchiveState state{limits,{},{},ControlCodecStatus::ok,true,false};
    WireControlMessage temporary;
    // Also run the ordinary generated nanopb decoder before committing storage.
    // Callback fields are skipped here; the strict descriptor pass above has
    // validated their complete nested messages and exact capacities.
    if(!preflight_protobuf(source,arena,state,temporary,result))return result;
    if(!materialize_protobuf(source,arena,state,temporary)){result={};result.status=ControlCodecStatus::codec_failure;return result;}
    output=temporary;result.consumed=source.size();return result;
}
} // namespace bdsp_protocol
