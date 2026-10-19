#include <bdsp_protocol/control_codec.h>
#include <cbor.h>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace bdsp_protocol {
namespace {
using namespace detail;
class FieldCounter final : public ControlArchive {
public:
    std::size_t fields=0;
    using ControlArchive::ControlArchive;
    bool uint_field(unsigned,std::uint64_t&) noexcept override { ++fields; return true; }
    bool optional_uint_field(unsigned,std::uint64_t&,bool& present) noexcept override { if(present)++fields; return true; }
    bool int_field(unsigned,std::int32_t&) noexcept override { ++fields; return true; }
    bool double_field(unsigned,double&) noexcept override { ++fields; return true; }
    bool bool_field(unsigned,bool&) noexcept override { ++fields; return true; }
    bool text_field(unsigned,std::string_view&,std::size_t) noexcept override { ++fields; return true; }
    bool object_field(unsigned,ControlSchema,void*,ControlVisit,bool* present) noexcept override {
        if(!present || *present)++fields;
        return true;
    }
    bool sequence_field(unsigned,ControlSchema,const void*,std::size_t&,ControlElementVisit) noexcept override {
        ++fields; return true;
    }
};
class CborWriter final : public ControlArchive {
    CborEncoder& encoder_;
    std::size_t depth_;
    bool sizing_, scalar_;
    bool ok(CborError error) noexcept {
        return error==CborNoError || (sizing_ && error==CborErrorOutOfMemory) || state.fail(ControlCodecStatus::codec_failure);
    }
    bool key(unsigned tag) noexcept { return scalar_ || ok(cbor_encode_uint(&encoder_,tag)); }
    bool sequence_element(CborEncoder& array,ControlSchema schema,const void* data,std::size_t index,
                          ControlElementVisit visit) noexcept {
        const bool scalar=schema==ControlSchema::TextElement || schema==ControlSchema::TypeElement;
        if(scalar) {
            CborWriter element(state,array,depth_+1,sizing_,true);
            return visit(element,data,index);
        }
        FieldCounter counter(state);
        if (!visit(counter,data,index)) return false;
        if(depth_+2>state.limits.max_depth) return state.fail(ControlCodecStatus::limit_exceeded);
        CborEncoder map;
        if (!ok(cbor_encoder_create_map(&array,&map,counter.fields))) return false;
        CborWriter element(state,map,depth_+2,sizing_);
        return visit(element,data,index) && ok(cbor_encoder_close_container_checked(&array,&map));
    }
public:
    CborWriter(ControlArchiveState& state,CborEncoder& encoder,std::size_t depth,bool sizing,bool scalar=false) noexcept
        : ControlArchive(state),encoder_(encoder),depth_(depth),sizing_(sizing),scalar_(scalar) {}
    bool uint_field(unsigned tag,std::uint64_t& value) noexcept override {
        return key(tag) && ok(cbor_encode_uint(&encoder_,value));
    }
    bool optional_uint_field(unsigned tag,std::uint64_t& value,bool& present) noexcept override {
        return !present || uint_field(tag,value);
    }
    bool int_field(unsigned tag,std::int32_t& value) noexcept override { return key(tag) && ok(cbor_encode_int(&encoder_,value)); }
    bool double_field(unsigned tag,double& value) noexcept override { return key(tag) && ok(cbor_encode_double(&encoder_,value)); }
    bool bool_field(unsigned tag,bool& value) noexcept override { return key(tag) && ok(cbor_encode_boolean(&encoder_,value)); }
    bool text_field(unsigned tag,std::string_view& value,std::size_t) noexcept override {
        return key(tag) && ok(cbor_encode_text_string(&encoder_,value.empty()?"":value.data(),value.size()));
    }
    bool map(ControlSchema schema,void* value,ControlVisit visit) noexcept {
        (void)schema;
        if (depth_>state.limits.max_depth) return state.fail(ControlCodecStatus::limit_exceeded);
        FieldCounter counter(state);
        if (!visit(counter,value)) return false;
        CborEncoder child;
        if (!ok(cbor_encoder_create_map(&encoder_,&child,counter.fields))) return false;
        CborWriter archive(state,child,depth_,sizing_);
        if (!visit(archive,value)) return false;
        return ok(cbor_encoder_close_container_checked(&encoder_,&child));
    }
    bool object_field(unsigned tag,ControlSchema schema,void* value,ControlVisit visit,bool* present) noexcept override {
        if (present && !*present) return true;
        if (!key(tag)) return false;
        CborWriter child(state,encoder_,depth_+1,sizing_);
        return child.map(schema,value,visit);
    }
    bool sequence_field(unsigned tag,ControlSchema schema,const void* data,std::size_t& count,ControlElementVisit visit) noexcept override {
        if (depth_+1>state.limits.max_depth) return state.fail(ControlCodecStatus::limit_exceeded);
        if (!key(tag)) return false;
        CborEncoder array;
        if (!ok(cbor_encoder_create_array(&encoder_,&array,count))) return false;
        for(std::size_t i=0;i<count;++i)
            if(!sequence_element(array,schema,data,i,visit))return false;
        return ok(cbor_encoder_close_container_checked(&encoder_,&array));
    }
};

// Bound nesting before TinyCBOR's generic recursive advance/validation is used.
bool bounded_value(CborValue& value,std::size_t depth,ControlArchiveState& state) noexcept;
bool bounded_container(CborValue& value,std::size_t depth,ControlArchiveState& state) noexcept {
    if(depth>state.limits.max_depth || depth>control_codec_max_depth) return state.fail(ControlCodecStatus::limit_exceeded);
    if(!cbor_value_is_length_known(&value)) return state.fail();
    CborValue child;
    if(cbor_value_enter_container(&value,&child)!=CborNoError)return state.fail();
    while(!cbor_value_at_end(&child))
        if(!bounded_value(child,depth+1,state))return false;
    return cbor_value_leave_container(&value,&child)==CborNoError || state.fail();
}
bool bounded_value(CborValue& value,std::size_t depth,ControlArchiveState& state) noexcept {
    if(cbor_value_is_container(&value)) return bounded_container(value,depth,state);
    if(cbor_value_is_tag(&value) || cbor_value_is_byte_string(&value))return state.fail();
    return cbor_value_advance(&value)==CborNoError || state.fail();
}
class CborReader final : public ControlArchive {
    std::array<CborValue,21> values_{};
    std::uint32_t present_=0,used_=0;
    ControlSchema schema_;
    bool take(unsigned tag,CborValue& value,bool required=true) noexcept {
        const auto bit=std::uint32_t{1}<<tag;
        if(!(present_&bit)) return !required || state.fail();
        used_|=bit; value=values_[tag]; return true;
    }
    bool read_field(CborValue& child) noexcept {
        std::uint64_t tag=0;
        if(!cbor_value_is_unsigned_integer(&child) || cbor_value_get_uint64(&child,&tag)!=CborNoError) return state.fail();
        if(tag==0 || tag>control_schema_max_tag(schema_)) return state.fail(ControlCodecStatus::unsupported_field);
        const auto bit=std::uint32_t{1}<<tag;
        if(present_&bit) return state.fail();
        if(cbor_value_advance_fixed(&child)!=CborNoError || cbor_value_at_end(&child)) return state.fail();
        present_|=bit;
        values_[tag]=child;
        return cbor_value_advance(&child)==CborNoError || state.fail();
    }
    bool read_sequence_element(CborValue& child,ControlSchema schema,const void* data,
                               std::size_t index,ControlElementVisit visit) noexcept {
        CborReader element(state,child,schema);
        if(state.error!=ControlCodecStatus::ok || !visit(element,data,index) || !element.finish()) return false;
        return cbor_value_advance(&child)==CborNoError || state.fail();
    }
    void read_map(const CborValue& value) noexcept {
        if(!cbor_value_is_map(&value) || !cbor_value_is_length_known(&value)){
            state.fail();return;
        }
        CborValue child;
        if(cbor_value_enter_container(&value,&child)!=CborNoError){state.fail();return;}
        while(!cbor_value_at_end(&child))if(!read_field(child))return;
    }
    std::size_t sequence_limit(ControlSchema schema) const noexcept {
        if(schema==ControlSchema::Node)return state.limits.max_nodes;
        if(schema==ControlSchema::Edge)return state.limits.max_edges;
        if(schema==ControlSchema::TextElement)return state.limits.max_capability_kinds;
        return state.limits.max_capability_types;
    }
public:
    CborReader(ControlArchiveState& state,const CborValue& value,ControlSchema schema) noexcept
        : ControlArchive(state),schema_(schema) {
        if(schema==ControlSchema::TextElement || schema==ControlSchema::TypeElement) {
            values_[0]=value;present_=1;return;
        }
        read_map(value);
    }
    bool finish() noexcept { return state.error==ControlCodecStatus::ok && (present_==used_ || state.fail()); }
    bool uint_field(unsigned tag,std::uint64_t& value) noexcept override {
        CborValue item{};
        return take(tag,item) && ((cbor_value_is_unsigned_integer(&item) &&
            cbor_value_get_uint64(&item,&value)==CborNoError) || state.fail());
    }
    bool optional_uint_field(unsigned tag,std::uint64_t& value,bool& present) noexcept override {
        present=(present_&(std::uint32_t{1}<<tag))!=0;
        return !present || uint_field(tag,value);
    }
    bool int_field(unsigned tag,std::int32_t& value) noexcept override {
        CborValue item{};std::int64_t number=0;
        if(!take(tag,item))return false;
        if(!cbor_value_is_integer(&item) || cbor_value_get_int64_checked(&item,&number)!=CborNoError ||
            number<std::numeric_limits<std::int32_t>::min() || number>std::numeric_limits<std::int32_t>::max())return state.fail();
        value=static_cast<std::int32_t>(number);return true;
    }
    bool double_field(unsigned tag,double& value) noexcept override {
        CborValue item{};
        return take(tag,item) && ((cbor_value_is_double(&item) && cbor_value_get_double(&item,&value)==CborNoError &&
            std::isfinite(value)) || state.fail());
    }
    bool bool_field(unsigned tag,bool& value) noexcept override {
        CborValue item{};
        return take(tag,item) && ((cbor_value_is_boolean(&item) && cbor_value_get_boolean(&item,&value)==CborNoError) || state.fail());
    }
    bool text_field(unsigned tag,std::string_view& value,std::size_t maximum) noexcept override {
        CborValue item{},next{}; const char* text=nullptr;std::size_t length=0;
        if(!take(tag,item))return false;
        if(!cbor_value_is_text_string(&item) || !cbor_value_is_length_known(&item) ||
            cbor_value_begin_string_iteration(&item)!=CborNoError ||
            cbor_value_get_text_string_chunk(&item,&text,&length,&next)!=CborNoError)return state.fail();
        value={text,length};return state.store_text(value,maximum);
    }
    bool object_field(unsigned tag,ControlSchema schema,void* value,ControlVisit visit,bool* present) noexcept override {
        if(present) { *present=(present_&(std::uint32_t{1}<<tag))!=0; if(!*present)return true; }
        CborValue item{};
        if(!take(tag,item))return false;
        CborReader child(state,item,schema);
        return state.error==ControlCodecStatus::ok && visit(child,value) && child.finish();
    }
    bool sequence_field(unsigned tag,ControlSchema schema,const void* data,std::size_t& count,ControlElementVisit visit) noexcept override {
        CborValue item{},child{};
        if(!take(tag,item))return false;
        if(!cbor_value_is_array(&item) || cbor_value_get_array_length(&item,&count)!=CborNoError)return state.fail();
        if(count>sequence_limit(schema))return state.fail(ControlCodecStatus::limit_exceeded);
        if(cbor_value_enter_container(&item,&child)!=CborNoError)return state.fail();
        for(std::size_t i=0;i<count;++i) if(!read_sequence_element(child,schema,data,i,visit)) return false;
        return cbor_value_at_end(&child) || state.fail();
    }
};
bool read_cbor(std::span<const std::uint8_t> bytes,ControlArchiveState& state,WireControlMessage& message,bool preflight) noexcept {
    CborParser parser;CborValue root;
    if(cbor_parser_init(bytes.data(),bytes.size(),0,&parser,&root)!=CborNoError)return state.fail();
    if(preflight) {
        auto bounded=root;
        if(!bounded_value(bounded,1,state))return false;
        if(cbor_value_get_next_byte(&bounded)!=bytes.data()+bytes.size())return state.fail();
        const auto flags=CborValidateShortestIntegrals|CborValidateNoIndeterminateLength|CborValidateUtf8|
            CborValidateNoTags|CborValidateFiniteFloatingPoint;
        if(cbor_value_validate(&root,flags)!=CborNoError)return state.fail();
    }
    CborReader archive(state,root,ControlSchema::ControlMessage);
    return state.error==ControlCodecStatus::ok && visit_control_message(archive,&message) && archive.finish();
}
bool measure_cbor(ControlArchiveState& state,WireControlMessage& message,std::size_t& size) noexcept {
    CborEncoder encoder;
    cbor_encoder_init(&encoder,nullptr,0,0);
    CborWriter measure(state,encoder,1,true);
    if(!measure.map(ControlSchema::ControlMessage,&message,visit_control_message)) return false;
    size=cbor_encoder_get_extra_bytes_needed(&encoder);
    return true;
}
bool emit_cbor(ControlArchiveState& state,WireControlMessage& message,std::span<std::uint8_t> output,
               std::span<std::uint8_t> scratch,std::size_t size) noexcept {
    CborEncoder encoder;
    cbor_encoder_init(&encoder,scratch.data(),scratch.size(),0);
    CborWriter writer(state,encoder,1,false);
    if(!writer.map(ControlSchema::ControlMessage,&message,visit_control_message)) return false;
    if(cbor_encoder_get_buffer_size(&encoder,scratch.data())!=size) return state.fail(ControlCodecStatus::codec_failure);
    std::memcpy(output.data(),scratch.data(),size);
    return true;
}
bool materialize_cbor(std::span<const std::uint8_t> source,ControlArchiveState& state,
                      ControlDecodeArena arena,WireControlMessage& message) noexcept {
    state.arena=arena;
    state.used={};
    state.materializing=true;
    message={};
    return read_cbor(source,state,message,false);
}
bool preflight_cbor(std::span<const std::uint8_t> source,ControlDecodeArena arena,
                    ControlArchiveState& state,WireControlMessage& message,ControlCodecResult& result) noexcept {
    if(source.size()>state.limits.max_wire_bytes){result.status=ControlCodecStatus::limit_exceeded;return false;}
    if(source.empty()){result.status=ControlCodecStatus::malformed;return false;}
    if(!read_cbor(source,state,message,true)){result.status=state.error;return false;}
    result.required_arena=state.used;
    if(!control_arena_fits(state.used,arena)){result.status=ControlCodecStatus::buffer_too_small;return false;}
    return true;
}
ControlCodecResult cbor_capacity(std::size_t size,std::span<std::uint8_t> output,
                                  std::span<std::uint8_t> scratch,const ControlCodecLimits& limits) noexcept {
    ControlCodecResult result;
    if(size>limits.max_wire_bytes){result.status=ControlCodecStatus::limit_exceeded;return result;}
    result.required_wire_bytes=size;
    result.required_scratch_bytes=size;
    if(output.size()<size || scratch.size()<size)result.status=ControlCodecStatus::buffer_too_small;
    return result;
}
ControlCodecResult encode_validated_cbor(const WireControlMessage& source,
    std::span<std::uint8_t> output,std::span<std::uint8_t> scratch,
    const ControlCodecLimits& limits) noexcept {
    WireControlMessage message=source;
    ControlArchiveState state{limits,{},{},ControlCodecStatus::ok,false,false};
    std::size_t size=0;
    if(!measure_cbor(state,message,size))return {state.error};
    auto result=cbor_capacity(size,output,scratch,limits);
    if(!result)return result;
    if(!emit_cbor(state,message,output,scratch,size))return {state.error};
    result.produced=size;
    return result;
}
}
ControlCodecResult CborControlCodec::encode(const WireControlMessage& source,std::span<std::uint8_t> output,
    std::span<std::uint8_t> scratch,const ControlCodecLimits& limits) const noexcept {
    auto result=validate_control_message(source,limits);
    if(!result)return result;
    return encode_validated_cbor(source,output,scratch,limits);
}
ControlCodecResult CborControlCodec::decode(std::span<const std::uint8_t> source,ControlDecodeArena arena,
    WireControlMessage& output,const ControlCodecLimits& limits) const noexcept {
    ControlCodecResult result;
    ControlArchiveState state{limits,{},{},ControlCodecStatus::ok,true,false};
    WireControlMessage temporary;
    if(!preflight_cbor(source,arena,state,temporary,result))return result;
    // Stable disjoint source: this repeats the exact validated traversal with
    // admitted storage; all refusal paths were evaluated before any writes.
    if(!materialize_cbor(source,state,arena,temporary)){result={};result.status=ControlCodecStatus::codec_failure;return result;}
    output=temporary;result.consumed=source.size();return result;
}
} // namespace bdsp_protocol
