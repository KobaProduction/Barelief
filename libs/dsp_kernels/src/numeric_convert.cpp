#include <dsp_kernels/numeric_convert.h>
#include "numeric_arithmetic.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <type_traits>

namespace dsp_kernels {
namespace {
using namespace signal_contracts;
using namespace numeric_detail;
constexpr auto size_max=std::numeric_limits<std::size_t>::max();
bool product(std::size_t a,std::size_t b,std::size_t& out) noexcept {
    if(b && a>size_max/b)return false;
    out=a*b;return true;
}
unsigned width(DType type) noexcept {return dtype_descriptor(type)->bytes*8;}
bool floating(DType type) noexcept {return dtype_descriptor(type)->ieee;}
bool signed_type(DType type) noexcept {return dtype_descriptor(type)->signed_value;}
unsigned valid_bits(const NumericContract& c) noexcept {return c.representation.valid_bits?c.representation.valid_bits:width(c.dtype);}
template<DType Type> constexpr unsigned typed_width() noexcept {
    if constexpr(Type==DType::Int8 || Type==DType::UInt8)return 8;
    else if constexpr(Type==DType::Int16 || Type==DType::UInt16)return 16;
    else if constexpr(Type==DType::Int32 || Type==DType::UInt32 || Type==DType::Float32)return 32;
    else return 64;
}
template<DType Type> constexpr bool typed_signed=Type!=DType::UInt8 && Type!=DType::UInt16 && Type!=DType::UInt32 && Type!=DType::UInt64;
template<DType Type> constexpr bool typed_floating=Type==DType::Float32 || Type==DType::Float64;
template<DType Type> Decoded decode(std::uint64_t bits) noexcept {
    if constexpr(Type==DType::Float32)return decode_ieee(bits,8,23);
    else if constexpr(Type==DType::Float64)return decode_ieee(bits,11,52);
    else return decode_integer(bits,typed_width<Type>(),typed_signed<Type>);
}
Decoded literal(const NumericLiteral& x) noexcept {
    return std::visit([](auto value) noexcept {
        using T=decltype(value);
        if constexpr(std::is_same_v<T,std::int64_t>)return decode_integer(std::bit_cast<std::uint64_t>(value),64,true);
        else if constexpr(std::is_same_v<T,std::uint64_t>)return decode_integer(value,64,false);
        else if constexpr(std::is_same_v<T,float>)return decode_ieee(std::bit_cast<std::uint32_t>(value),8,23);
        else return decode_ieee(std::bit_cast<std::uint64_t>(value),11,52);
    },x);
}
bool overlap(std::span<const std::byte> a,std::span<const std::byte> b) noexcept {
    if(a.empty() || b.empty())return false;
    const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());
    return x<=y?y-x<a.size():x-y<b.size();
}
NumericStatus map_status(Status s) noexcept {
    if(s==Status::ok)return NumericStatus::Ok;
    if(s==Status::exhausted || s==Status::overflow)return NumericStatus::LimitExceeded;
    if(s==Status::buffer_too_small)return NumericStatus::BufferTooSmall;
    return NumericStatus::InvalidLayout;
}
void snapshot(const NumericContract& c,KernelContract& out) noexcept {
    out.value=c;out.plane_count=c.channel_order.size();out.clock_size=c.clock.domain.size();
    std::copy(c.channel_order.begin(),c.channel_order.end(),out.channels.begin());
    std::copy(c.clock.domain.begin(),c.clock.domain.end(),out.clock.begin());
    out.value.channel_order={};out.value.clock.domain={};
}
bool same_geometry(const NumericContract& a,const NumericContract& b) noexcept {
    auto adapted=a;adapted.dtype=b.dtype;adapted.representation=b.representation;
    adapted.unit=b.unit;
    return same_physical_contract(DataContract{adapted},DataContract{b});
}
bool enum_policy(const ConversionPolicy& p) noexcept {
    return (p.rounding==Rounding::TowardZero || p.rounding==Rounding::NearestTiesEven) &&
        (p.overflow==Overflow::Reject || p.overflow==Overflow::Saturate) &&
        (p.precision_loss==PrecisionLoss::RejectInexact || p.precision_loss==PrecisionLoss::AllowDeclared) &&
        (p.nonfinite==NonFinite::Reject || p.nonfinite==NonFinite::ReplaceFinite || p.nonfinite==NonFinite::ClampFinite) &&
        (p.scaling.mode==ScaleMode::Identity || p.scaling.mode==ScaleMode::IntegerAffine || p.scaling.mode==ScaleMode::DyadicAffine ||
         p.scaling.mode==ScaleMode::PcmNormalize || p.scaling.mode==ScaleMode::PcmDenormalize || p.scaling.mode==ScaleMode::Binary64Affine);
}
NumericStatus nonfinite_policy_status(const ConversionPolicy& p) noexcept {
    if(validate_literal(p.scaling.gain.numerator)!=Status::ok || validate_literal(p.scaling.offset.numerator)!=Status::ok ||
       validate_literal(p.nan_replacement)!=Status::ok || validate_literal(p.negative_inf_replacement)!=Status::ok ||
       validate_literal(p.positive_inf_replacement)!=Status::ok)return NumericStatus::InvalidPolicy;
    const bool allow=p.precision_loss==PrecisionLoss::AllowDeclared;
    if((p.overflow==Overflow::Saturate || p.nonfinite!=NonFinite::Reject) && !allow)return NumericStatus::InvalidPolicy;
    if(p.nonfinite!=NonFinite::Reject &&
       (literal(p.nan_replacement).special!=Special::Finite || literal(p.negative_inf_replacement).special!=Special::Finite ||
        literal(p.positive_inf_replacement).special!=Special::Finite))return NumericStatus::InvalidPolicy;
    if(p.nonfinite==NonFinite::ClampFinite && compare_binary(literal(p.negative_inf_replacement).value,literal(p.positive_inf_replacement).value)>0)
        return NumericStatus::InvalidPolicy;
    return NumericStatus::Ok;
}
NumericStatus exact_affine_policy_status(const ConversionRequest& r) noexcept {
    const auto& scaling=r.policy.scaling;
    if(scaling.gain.numerator.index()>1 || scaling.offset.numerator.index()>1 ||
       floating(r.input.dtype))return NumericStatus::UnsupportedCombination;
    if(scaling.mode==ScaleMode::IntegerAffine &&
       (scaling.gain.exponent2 || scaling.offset.exponent2))return NumericStatus::InvalidPolicy;
    if(scaling.gain.exponent2 < -63 || scaling.gain.exponent2 > 63 ||
       scaling.offset.exponent2 < -63 || scaling.offset.exponent2 > 63)
        return NumericStatus::UnsupportedCombination;
    return NumericStatus::Ok;
}

NumericStatus binary64_affine_policy_status(const ConversionRequest& r) noexcept {
    const auto& scaling=r.policy.scaling;
    if(!declared_ieee64_environment_available)return NumericStatus::UnsupportedCombination;
    if(!floating(r.input.dtype) || !floating(r.output.dtype) ||
       r.policy.precision_loss!=PrecisionLoss::AllowDeclared)return NumericStatus::UnsupportedCombination;
    if(literal(scaling.gain.numerator).special!=Special::Finite ||
       literal(scaling.offset.numerator).special!=Special::Finite)return NumericStatus::InvalidPolicy;
    if(scaling.gain.exponent2 < -63 || scaling.gain.exponent2 > 63 ||
       scaling.offset.exponent2 < -63 || scaling.offset.exponent2 > 63)
        return NumericStatus::UnsupportedCombination;
    return NumericStatus::Ok;
}

NumericStatus affine_policy_status(const ConversionRequest& r) noexcept {
    const auto mode=r.policy.scaling.mode;
    if(mode==ScaleMode::IntegerAffine || mode==ScaleMode::DyadicAffine)
        return exact_affine_policy_status(r);
    if(mode==ScaleMode::Binary64Affine)return binary64_affine_policy_status(r);
    return NumericStatus::Ok;
}
NumericStatus pcm_policy_status(const ConversionRequest& r) noexcept {
    const auto& p=r.policy;
    if(p.scaling.mode==ScaleMode::PcmNormalize) {
        if(floating(r.input.dtype) || !floating(r.output.dtype) ||
           r.output.representation.normalization!=Normalization::FloatUnit)return NumericStatus::InvalidPolicy;
    } else if(p.scaling.mode==ScaleMode::PcmDenormalize) {
        if(!floating(r.input.dtype) || floating(r.output.dtype) ||
           r.input.representation.normalization!=Normalization::FloatUnit)return NumericStatus::InvalidPolicy;
    } else if(r.input.unit!=r.output.unit || r.input.representation.normalization!=r.output.representation.normalization)
        return NumericStatus::InvalidPolicy;
    return NumericStatus::Ok;
}
NumericStatus policy_status(const ConversionRequest& r) noexcept {
    if(!enum_policy(r.policy))return NumericStatus::InvalidPolicy;
    const auto nonfinite=nonfinite_policy_status(r.policy);if(nonfinite!=NumericStatus::Ok)return nonfinite;
    const auto affine=affine_policy_status(r);if(affine!=NumericStatus::Ok)return affine;
    return pcm_policy_status(r);
}
struct ElementResult {NumericStatus status=NumericStatus::Ok;std::uint64_t bits=0;std::uint32_t loss=0;};
template<DType To> ElementResult encode_floating(Binary value,const ConversionPolicy& p) noexcept {
    const bool allow=p.precision_loss==PrecisionLoss::AllowDeclared,nearest=p.rounding==Rounding::NearestTiesEven;
    const auto packed=pack_ieee(value,To==DType::Float32?8:11,To==DType::Float32?23:52,nearest,allow);
    if(packed.error==ArithmeticError::Inexact)return {NumericStatus::PrecisionLoss};
    if(packed.error==ArithmeticError::Overflow) {
        if(p.overflow==Overflow::Reject)return {NumericStatus::Overflow};
        const auto maximum=To==DType::Float32?UINT64_C(0x7f7fffff):UINT64_C(0x7fefffffffffffff);
        return {NumericStatus::Ok,maximum|(std::uint64_t(value.negative)<<(typed_width<To>()-1)),std::uint32_t(LossFlag::Saturated)};
    }
    return {NumericStatus::Ok,packed.bits,(packed.rounded?std::uint32_t(LossFlag::Rounded):0)|(packed.underflow?std::uint32_t(LossFlag::Underflow):0)};
}
template<DType To> ElementResult saturated_integer(Binary value,unsigned bits) noexcept {
    constexpr bool sign=typed_signed<To>;
    const auto bound=sign?(value.negative?(std::uint64_t{1}<<(bits-1)):
        (std::uint64_t{1}<<(bits-1))-1):bits==64?UINT64_MAX:(std::uint64_t{1}<<bits)-1;
    const auto result=!sign && value.negative?0:value.negative?~bound+1:bound;
    return {NumericStatus::Ok,result,std::uint32_t(LossFlag::Saturated)};
}

template<DType To> ElementResult encode_integer(Binary value,unsigned bits,const ConversionPolicy& p) noexcept {
    const bool allow=p.precision_loss==PrecisionLoss::AllowDeclared;
    const bool nearest=p.rounding==Rounding::NearestTiesEven;
    const auto packed=pack_integer(value,bits,typed_signed<To>,nearest,allow);
    if(packed.error==ArithmeticError::Overflow) {
        if(p.overflow==Overflow::Reject)return {NumericStatus::Overflow};
        return saturated_integer<To>(value,bits);
    }
    if(packed.error==ArithmeticError::Inexact)return {NumericStatus::PrecisionLoss};
    return {NumericStatus::Ok,packed.bits,packed.rounded?std::uint32_t(LossFlag::Rounded):0};
}
template<DType To> ElementResult encode(Binary value,const NumericContract& c,const ConversionPolicy& p) noexcept {
    if constexpr(typed_floating<To>)return encode_floating<To>(value,p);
    else return encode_integer<To>(value,c.representation.valid_bits?c.representation.valid_bits:typed_width<To>(),p);
}
NumericStatus normalize_pcm(Binary& value,const NumericContract& input) noexcept {
    if(!signed_type(input.dtype)) {
        Binary bias{Magnitude::from(std::uint64_t{1}<<(valid_bits(input)-1)),0,true};
        if(!add_binary(value,bias))return NumericStatus::Overflow;
    }
    value.exponent2-=int(valid_bits(input))-1;
    return NumericStatus::Ok;
}
template<DType To> ElementResult denormalize_pcm(Binary value,const NumericContract& output,
                                                 const ConversionPolicy& policy) noexcept {
    value.exponent2+=int(valid_bits(output))-1;
    if(!signed_type(output.dtype)) {
        Binary bias{Magnitude::from(std::uint64_t{1}<<(valid_bits(output)-1)),0,false};
        if(!add_binary(value,bias)) {
            // Alignment failure for a tiny input is decided at half a unit.
            const int leading=int(value.magnitude.bits())-1+value.exponent2;
            if(leading>=-1)return encode<To>(value,output,policy);
            if(policy.precision_loss==PrecisionLoss::RejectInexact)return {NumericStatus::PrecisionLoss};
            auto result=bias.magnitude.low();
            if(value.negative && policy.rounding==Rounding::TowardZero)--result;
            return {NumericStatus::Ok,result,std::uint32_t(LossFlag::Rounded)};
        }
    }
    return encode<To>(value,output,policy);
}
NumericStatus exact_affine(Binary& value,const ConversionPolicy& policy) noexcept {
    auto gain=literal(policy.scaling.gain.numerator).value;
    auto offset=literal(policy.scaling.offset.numerator).value;
    gain.exponent2+=policy.scaling.gain.exponent2;
    offset.exponent2+=policy.scaling.offset.exponent2;
    if(!value.magnitude.multiply(gain.magnitude))return NumericStatus::Overflow;
    value.exponent2+=gain.exponent2;
    value.negative=value.negative!=gain.negative;
    return add_binary(value,offset)?NumericStatus::Ok:NumericStatus::Overflow;
}
NumericStatus apply_binary64_packed(const Packed& a,const Packed& b,const Packed& c,
                                     Binary& value,std::uint32_t& loss) noexcept {
#if DSP_KERNELS_HAS_DECLARED_IEEE64_ENVIRONMENT
    if(a.error!=ArithmeticError::Ok || b.error!=ArithmeticError::Ok ||
       c.error!=ArithmeticError::Ok)return NumericStatus::Overflow;
    NearestEnvironment environment;
    if(!environment.ready)return NumericStatus::BackendFailure;
    volatile double product_value=std::bit_cast<double>(a.bits)*std::bit_cast<double>(b.bits);
    volatile double affine_value=product_value+std::bit_cast<double>(c.bits);
    const auto decoded=decode_ieee(std::bit_cast<std::uint64_t>(double(affine_value)),11,52);
    if(decoded.special!=Special::Finite)return NumericStatus::Overflow;
    value=decoded.value;
    if(a.rounded || b.rounded || c.rounded || std::fetestexcept(FE_INEXACT))loss|=std::uint32_t(LossFlag::Rounded);
    if(a.underflow || b.underflow || c.underflow || std::fetestexcept(FE_UNDERFLOW))loss|=std::uint32_t(LossFlag::Underflow);
    return NumericStatus::Ok;
#else
    (void)a;(void)b;(void)c;(void)value;(void)loss;
    return NumericStatus::UnsupportedCombination;
#endif
}

NumericStatus binary64_affine(Binary& value,const ConversionPolicy& policy,std::uint32_t& loss) noexcept {
#if DSP_KERNELS_HAS_DECLARED_IEEE64_ENVIRONMENT
    auto gain=literal(policy.scaling.gain.numerator).value;
    auto offset=literal(policy.scaling.offset.numerator).value;
    gain.exponent2+=policy.scaling.gain.exponent2;
    offset.exponent2+=policy.scaling.offset.exponent2;
    const auto a=pack_ieee(value,11,52,true,true);
    const auto b=pack_ieee(gain,11,52,true,true);
    const auto c=pack_ieee(offset,11,52,true,true);
    return apply_binary64_packed(a,b,c,value,loss);
#else
    (void)value;(void)policy;(void)loss;
    return NumericStatus::UnsupportedCombination;
#endif
}
NumericStatus replace_nonfinite(Decoded& input,const ConversionPolicy& policy,
                                std::uint32_t& loss) noexcept {
    if(input.special==Special::Finite)return NumericStatus::Ok;
    if(policy.nonfinite==NonFinite::Reject)return NumericStatus::NonFiniteInput;
    input=literal(input.special==Special::NaN?policy.nan_replacement:
        input.special==Special::NegativeInfinity?policy.negative_inf_replacement:
        policy.positive_inf_replacement);
    loss=std::uint32_t(LossFlag::ReplacedNonFinite);
    return NumericStatus::Ok;
}

NumericStatus apply_conversion_scaling(Binary& value,const NumericContract& input,
                                       const ConversionPolicy& policy,std::uint32_t& loss) noexcept {
    if(policy.scaling.mode==ScaleMode::PcmNormalize)return normalize_pcm(value,input);
    if(policy.scaling.mode==ScaleMode::IntegerAffine ||
       policy.scaling.mode==ScaleMode::DyadicAffine)return exact_affine(value,policy);
    if(policy.scaling.mode==ScaleMode::Binary64Affine)return binary64_affine(value,policy,loss);
    return NumericStatus::Ok;
}

template<DType To> ElementResult element(Decoded input,const NumericContract& in,
                                         const NumericContract& out,const ConversionPolicy& p) noexcept {
    std::uint32_t loss=0;
    const auto special=replace_nonfinite(input,p,loss);
    if(special!=NumericStatus::Ok)return {special};
    auto value=input.value;
    if(p.scaling.mode==ScaleMode::PcmDenormalize) {
        auto result=denormalize_pcm<To>(value,out,p);
        result.loss|=loss;
        return result;
    }
    const auto scaled=apply_conversion_scaling(value,in,p,loss);
    if(scaled!=NumericStatus::Ok)return {scaled};
    auto result=encode<To>(value,out,p);
    result.loss|=loss;
    return result;
}
ElementResult prepare_element(Decoded input,const NumericContract& in,const NumericContract& out,const ConversionPolicy& p) noexcept {
#define D_ENTRY(T) case DType::T:return element<DType::T>(input,in,out,p)
    switch(out.dtype){D_ENTRY(Int8);D_ENTRY(UInt8);D_ENTRY(Int16);D_ENTRY(UInt16);D_ENTRY(Int32);D_ENTRY(UInt32);D_ENTRY(Int64);D_ENTRY(UInt64);D_ENTRY(Float32);D_ENTRY(Float64);}
#undef D_ENTRY
    return {NumericStatus::InvalidContract};
}
bool big(StorageEncoding e) noexcept {return e==StorageEncoding::FixedBigEndian || e==StorageEncoding::PackedSigned24BE ||
    (e==StorageEncoding::Native && std::endian::native==std::endian::big);}
template<DType To> void write_encoded(std::byte* out,std::size_t bytes,std::uint64_t bits,const NumericContract& c) noexcept {
    const auto n=c.representation.valid_bits?c.representation.valid_bits:typed_width<To>();
    const auto mask=n==64?UINT64_MAX:(std::uint64_t{1}<<n)-1;
    auto raw=bits&mask;
    if(!typed_floating<To> && n<bytes*8) {
        if(c.representation.placement==BitPlacement::MostSignificant)raw<<=unsigned(bytes*8)-n;
        else if(typed_signed<To> && ((raw>>(n-1))&1))raw|=(bytes==8?UINT64_MAX:(std::uint64_t{1}<<(bytes*8))-1)&~mask;
    }
    for(std::size_t i=0;i<bytes;++i)out[i]=std::byte((raw>>(8*(big(c.representation.encoding)?bytes-1-i:i)))&255);
}
// Dispatch fixes both dtype identities before the loop. R load_bits handles representation and semantic silence.
template<DType From,DType To> NumericResult stage_conversion_element(
    const PreparedConversion& plan,const NumericView& input,std::span<std::byte> scratch,
    const NumericContract& in,const NumericContract& out,
    std::size_t plane,std::size_t row,std::size_t column,std::size_t columns,
    std::size_t output_width,std::size_t& written,std::uint32_t& loss) noexcept {
    std::uint64_t bits=0;
    if(load_bits(input,{plane,row,column},From,bits)!=Status::ok)
        return {NumericStatus::InvalidLayout,0,plan.requirements.scratch_bytes,plane,row*columns+column};
    const auto value=element<To>(decode<From>(bits),in,out,plan.policy);
    if(value.status!=NumericStatus::Ok)
        return {value.status,0,plan.requirements.scratch_bytes,plane,row*columns+column};
    write_encoded<To>(scratch.data()+written*output_width,output_width,value.bits,out);
    ++written;loss|=value.loss;
    return {NumericStatus::Ok};
}
template<DType From,DType To> NumericResult stage_conversion(const PreparedConversion& plan,const NumericView& input,
                                                              std::span<std::byte> scratch,std::size_t rows,
                                                              std::size_t columns,std::size_t output_width) noexcept {
    const auto in=plan.input.contract(),out=plan.output.contract();
    std::size_t written=0;std::uint32_t loss=0;
    for(std::size_t plane=0;plane<input.planes.size();++plane)
        for(std::size_t row=0;row<rows;++row)
            for(std::size_t column=0;column<columns;++column) {
                const auto result=stage_conversion_element<From,To>(plan,input,scratch,in,out,plane,row,
                    column,columns,output_width,written,loss);
                if(result.status!=NumericStatus::Ok)return result;
            }
    return {NumericStatus::Ok,written*output_width,plan.requirements.scratch_bytes,size_max,size_max,loss};
}
std::size_t converted_offset(const NumericContract& contract,const MutableNumericView& output,
                             std::size_t plane,std::size_t row,std::size_t column) noexcept {
    const auto& layout=output.planes[plane];
    const auto physical=output.ring.capacity?(output.ring.oldest+row)%output.ring.capacity:row;
    return layout.offset+(contract.shape.rank==Rank::Scalar?0:physical*layout.stride_bytes[0])+
        (contract.shape.rank==Rank::Matrix?column*layout.stride_bytes[1]:0);
}
void publish_conversion(const NumericContract& contract,MutableNumericView& output,
                        std::span<const std::byte> staged,std::size_t rows,
                        std::size_t columns,std::size_t width) noexcept {
    std::size_t index=0;
    for(std::size_t plane=0;plane<output.planes.size();++plane)
        for(std::size_t row=0;row<rows;++row)
            for(std::size_t column=0;column<columns;++column) {
                const auto offset=converted_offset(contract,output,plane,row,column);
                std::memcpy(output.storage.data()+offset,staged.data()+index*width,width);
                ++index;
            }
}
template<DType From,DType To> NumericResult run(const PreparedConversion& plan,const NumericView& input,MutableNumericView& output,std::span<std::byte> scratch) noexcept {
    const auto out=plan.output.contract();
    const auto in=plan.input.contract();
    const auto rows=in.shape.rank==Rank::Scalar?1:input.actual[0];
    const auto columns=in.shape.rank==Rank::Matrix?input.actual[1]:1;
    std::size_t output_width=0;storage_width(out,output_width);
    const auto staged=stage_conversion<From,To>(plan,input,scratch,rows,columns,output_width);
    if(staged.status!=NumericStatus::Ok)return staged;
    // Every operation that can fail precedes this scatter; source/output aliasing is safe after staging.
    publish_conversion(out,output,scratch,rows,columns,output_width);
    return staged;
}
template<DType From> PreparedConversion::Function select_output(DType to) noexcept {
#define D_ENTRY(T) case DType::T:return &run<From,DType::T>
    switch(to){D_ENTRY(Int8);D_ENTRY(UInt8);D_ENTRY(Int16);D_ENTRY(UInt16);D_ENTRY(Int32);D_ENTRY(UInt32);D_ENTRY(Int64);D_ENTRY(UInt64);D_ENTRY(Float32);D_ENTRY(Float64);}
#undef D_ENTRY
    return nullptr;
}
PreparedConversion::Function select(DType from,DType to) noexcept {
#define D_ENTRY(T) case DType::T:return select_output<DType::T>(to)
    switch(from){D_ENTRY(Int8);D_ENTRY(UInt8);D_ENTRY(Int16);D_ENTRY(UInt16);D_ENTRY(Int32);D_ENTRY(UInt32);D_ENTRY(Int64);D_ENTRY(UInt64);D_ENTRY(Float32);D_ENTRY(Float64);}
#undef D_ENTRY
    return nullptr;
}
NumericStatus validate_conversion_contracts(const ConversionRequest& request,const KernelCapabilities& capabilities) noexcept {
    if(!capabilities.converter)return NumericStatus::UnsupportedCombination;
    for(const auto* contract:{&request.input,&request.output}) {
        const auto status=validate_contract(DataContract{*contract},request.frame_limits);
        if(status!=Status::ok)return status==Status::exhausted || status==Status::overflow?
            NumericStatus::LimitExceeded:NumericStatus::InvalidContract;
    }
    if(!same_geometry(request.input,request.output))return NumericStatus::InvalidContract;
    if((request.input.dtype==DType::Float64 || request.output.dtype==DType::Float64) &&
       !capabilities.float64_arithmetic)return NumericStatus::UnsupportedCombination;
    return NumericStatus::Ok;
}
NumericStatus validate_replacements(const ConversionRequest& request) noexcept {
    if(request.policy.nonfinite==NonFinite::Reject)return NumericStatus::Ok;
    for(const auto* replacement:{&request.policy.nan_replacement,&request.policy.negative_inf_replacement,
                                 &request.policy.positive_inf_replacement})
        if(prepare_element(literal(*replacement),request.input,request.output,request.policy).status!=NumericStatus::Ok)
            return NumericStatus::InvalidPolicy;
    return NumericStatus::Ok;
}
bool conversion_element_count(const NumericContract& input,std::size_t& count) noexcept {
    count=input.channel_order.size();
    const unsigned rank=input.shape.rank==Rank::Scalar?0:input.shape.rank==Rank::Vector?1:2;
    for(unsigned axis=0;axis<rank;++axis)
        if(!product(count,std::size_t(input.shape.extents[axis].maximum),count))return false;
    return true;
}

ConversionSupport conversion_resources(const ConversionRequest& request) noexcept {
    if(request.input.channel_order.size()>KernelContract::max_planes ||
       request.input.clock.domain.size()>KernelContract::max_clock_bytes ||
       request.input.channel_order.size()>request.limits.max_planes)return {NumericStatus::LimitExceeded,{}};
    std::size_t count=0;
    if(!conversion_element_count(request.input,count))return {NumericStatus::LimitExceeded,{}};
    if(count>request.limits.max_elements)return {NumericStatus::LimitExceeded,{}};
    std::size_t bytes=0,input_bytes=0,output_width=0,input_width=0;
    storage_width(request.input,input_width);storage_width(request.output,output_width);
    if(!product(count,output_width,bytes) || !product(count,input_width,input_bytes) ||
       bytes>request.limits.max_scratch_bytes || bytes>request.limits.max_output_bytes ||
       input_bytes>request.limits.max_input_bytes)return {NumericStatus::LimitExceeded,{}};
    return {NumericStatus::Ok,{bytes,1,sizeof(PreparedConversion),numeric_reference_stack_bytes}};
}
NumericResult validate_conversion_views(const PreparedConversion& plan,const NumericView& input,
                                        const MutableNumericView& output,std::span<const std::byte> scratch) noexcept {
    const auto in=plan.input.contract(),out=plan.output.contract();
    if(!input.contract || !output.contract || !same_physical_contract(DataContract{in},DataContract{*input.contract}) ||
       !same_physical_contract(DataContract{out},DataContract{*output.contract}) || input.actual!=output.actual)
        return {NumericStatus::InvalidContract};
    const auto source=map_status(validate_view(DataFrameView{{},input},plan.frame_limits));
    if(source!=NumericStatus::Ok)return {source};
    const auto target=map_status(validate_mutable(output,plan.frame_limits));
    if(target!=NumericStatus::Ok)return {target};
    if(input.storage.size()>plan.limits.max_input_bytes || output.storage.size()>plan.limits.max_output_bytes)
        return {NumericStatus::LimitExceeded};
    if(scratch.size()<plan.requirements.scratch_bytes)
        return {NumericStatus::BufferTooSmall,0,plan.requirements.scratch_bytes};
    return {NumericStatus::Ok};
}
bool conversion_metadata_alias(const PreparedConversion& plan,const NumericView& input,
                               const MutableNumericView& output,std::span<const std::byte> scratch) noexcept {
    const auto conflict=[&](std::span<const std::byte> metadata) noexcept {
        return overlap(scratch,metadata) || overlap(output.storage,metadata);
    };
    if(overlap(scratch,input.storage) || overlap(scratch,output.storage) ||
       conflict(std::as_bytes(std::span(&plan,1))) || conflict(std::as_bytes(std::span(&input,1))) ||
       conflict(std::as_bytes(std::span(&output,1))) ||
       conflict(std::as_bytes(std::span(input.contract,1))) ||
       conflict(std::as_bytes(std::span(output.contract,1))) ||
       conflict(std::as_bytes(input.planes)) || conflict(std::as_bytes(output.planes)) ||
       conflict(std::as_bytes(input.contract->channel_order)) ||
       conflict(std::as_bytes(output.contract->channel_order)) ||
       conflict(std::as_bytes(std::span(input.contract->clock.domain.data(),input.contract->clock.domain.size()))) ||
       conflict(std::as_bytes(std::span(output.contract->clock.domain.data(),output.contract->clock.domain.size()))) ||
       conflict(input.ring.silence_mask) || conflict(output.ring.silence_mask))return true;
    for(unsigned axis=0;axis<2;++axis)
        if(conflict(input.coordinates.axes[axis]) || conflict(output.coordinates.axes[axis]))return true;
    return false;
}
bool conversion_coordinates_match(const NumericView& input,const MutableNumericView& output) noexcept {
    for(unsigned axis=0;axis<2;++axis)
        if(input.coordinates.axes[axis].size()!=output.coordinates.axes[axis].size() ||
           !std::equal(input.coordinates.axes[axis].begin(),input.coordinates.axes[axis].end(),
                       output.coordinates.axes[axis].begin()))return false;
    return true;
}
}
ConversionSupport conversion_support(const ConversionRequest& r,const KernelCapabilities& caps) noexcept {
    const auto contracts=validate_conversion_contracts(r,caps);if(contracts!=NumericStatus::Ok)return {contracts,{}};
    const auto policy=policy_status(r);if(policy!=NumericStatus::Ok)return {policy,{}};
    if(r.policy.scaling.mode==ScaleMode::Binary64Affine && !caps.float64_arithmetic)return {NumericStatus::UnsupportedCombination,{}};
    const auto replacements=validate_replacements(r);if(replacements!=NumericStatus::Ok)return {replacements,{}};
    return conversion_resources(r);
}
NumericStatus prepare_conversion(const ConversionRequest& r,const KernelCapabilities& caps,PreparedConversion& out) noexcept {
    const auto support=conversion_support(r,caps);if(support.status!=NumericStatus::Ok)return support.status;
    PreparedConversion plan; snapshot(r.input,plan.input);snapshot(r.output,plan.output);
    plan.policy=r.policy;plan.limits=r.limits;plan.frame_limits=r.frame_limits;plan.requirements=support.requirements;plan.function=select(r.input.dtype,r.output.dtype);
    out=plan;return NumericStatus::Ok;
}
NumericResult convert(const PreparedConversion& plan,const NumericView& input,MutableNumericView& output,std::span<std::byte> scratch) noexcept {
    if(!plan.function)return {NumericStatus::UnsupportedCombination};
    const auto validation=validate_conversion_views(plan,input,output,scratch);
    if(validation.status!=NumericStatus::Ok)return validation;
    scratch=scratch.first(plan.requirements.scratch_bytes);
    if(conversion_metadata_alias(plan,input,output,scratch))return {NumericStatus::InvalidLayout};
    if(!conversion_coordinates_match(input,output))return {NumericStatus::InvalidContract};
    return plan.function(plan,input,output,scratch);
}
}
