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
NumericStatus policy_status(const ConversionRequest& r) noexcept {
    const auto& p=r.policy;
    if(!enum_policy(p))return NumericStatus::InvalidPolicy;
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
    if(p.scaling.mode==ScaleMode::IntegerAffine || p.scaling.mode==ScaleMode::DyadicAffine) {
        // Exact integer coefficients keep admission bounds independent of host floating arithmetic.
        if(p.scaling.gain.numerator.index()>1 || p.scaling.offset.numerator.index()>1 || floating(r.input.dtype))
            return NumericStatus::UnsupportedCombination;
        if(p.scaling.mode==ScaleMode::IntegerAffine && (p.scaling.gain.exponent2 || p.scaling.offset.exponent2))return NumericStatus::InvalidPolicy;
        if(p.scaling.gain.exponent2 < -63 || p.scaling.gain.exponent2 > 63 || p.scaling.offset.exponent2 < -63 || p.scaling.offset.exponent2 > 63)
            return NumericStatus::UnsupportedCombination;
    }
    if(p.scaling.mode==ScaleMode::Binary64Affine) {
        if(!declared_ieee64_environment_available)return NumericStatus::UnsupportedCombination;
        if(!floating(r.input.dtype) || !floating(r.output.dtype) || !allow)return NumericStatus::UnsupportedCombination;
        if(literal(p.scaling.gain.numerator).special!=Special::Finite || literal(p.scaling.offset.numerator).special!=Special::Finite)
            return NumericStatus::InvalidPolicy;
        if(p.scaling.gain.exponent2 < -63 || p.scaling.gain.exponent2 > 63 || p.scaling.offset.exponent2 < -63 || p.scaling.offset.exponent2 > 63)
            return NumericStatus::UnsupportedCombination;
    }
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
struct ElementResult {NumericStatus status=NumericStatus::Ok;std::uint64_t bits=0;std::uint32_t loss=0;};
template<DType To> ElementResult encode(Binary value,const NumericContract& c,const ConversionPolicy& p) noexcept {
    const bool allow=p.precision_loss==PrecisionLoss::AllowDeclared,nearest=p.rounding==Rounding::NearestTiesEven;
    const auto bits=c.representation.valid_bits?c.representation.valid_bits:typed_width<To>();
    if constexpr(typed_floating<To>) {
        const auto packed=pack_ieee(value,To==DType::Float32?8:11,To==DType::Float32?23:52,nearest,allow);
        if(packed.error==ArithmeticError::Inexact)return {NumericStatus::PrecisionLoss};
        if(packed.error==ArithmeticError::Overflow) {
            if(p.overflow==Overflow::Reject)return {NumericStatus::Overflow};
            const auto maximum=To==DType::Float32?UINT64_C(0x7f7fffff):UINT64_C(0x7fefffffffffffff);
            return {NumericStatus::Ok,maximum|(std::uint64_t(value.negative)<<(typed_width<To>()-1)),std::uint32_t(LossFlag::Saturated)};
        }
        return {NumericStatus::Ok,packed.bits,(packed.rounded?std::uint32_t(LossFlag::Rounded):0)|(packed.underflow?std::uint32_t(LossFlag::Underflow):0)};
    }
    const bool sign=typed_signed<To>;
    const auto packed=pack_integer(value,bits,sign,nearest,allow);
    if(packed.error==ArithmeticError::Overflow) {
        if(p.overflow==Overflow::Reject)return {NumericStatus::Overflow};
        const auto bound=sign?(value.negative?(std::uint64_t{1}<<(bits-1)):(std::uint64_t{1}<<(bits-1))-1):
            bits==64?UINT64_MAX:(std::uint64_t{1}<<bits)-1;
        const auto result=!sign && value.negative?0:value.negative?~bound+1:bound;
        return {NumericStatus::Ok,result,std::uint32_t(LossFlag::Saturated)};
    }
    if(packed.error==ArithmeticError::Inexact)return {NumericStatus::PrecisionLoss};
    return {NumericStatus::Ok,packed.bits,packed.rounded?std::uint32_t(LossFlag::Rounded):0};
}
template<DType To> ElementResult element(Decoded input,const NumericContract& in,const NumericContract& out,const ConversionPolicy& p) noexcept {
    std::uint32_t loss=0;
    if(input.special!=Special::Finite) {
        if(p.nonfinite==NonFinite::Reject)return {NumericStatus::NonFiniteInput};
        input=literal(input.special==Special::NaN?p.nan_replacement:input.special==Special::NegativeInfinity?p.negative_inf_replacement:p.positive_inf_replacement);
        loss=std::uint32_t(LossFlag::ReplacedNonFinite);
    }
    auto value=input.value;
    if(p.scaling.mode==ScaleMode::PcmNormalize) {
        if(!signed_type(in.dtype)) {
            Binary bias{Magnitude::from(std::uint64_t{1}<<(valid_bits(in)-1)),0,true};
            if(!add_binary(value,bias))return {NumericStatus::Overflow};
        }
        value.exponent2-=int(valid_bits(in))-1;
    } else if(p.scaling.mode==ScaleMode::PcmDenormalize) {
        value.exponent2+=int(valid_bits(out))-1;
        if(!signed_type(out.dtype)) {
            Binary bias{Magnitude::from(std::uint64_t{1}<<(valid_bits(out)-1)),0,false};
            if(!add_binary(value,bias)) {
                // A tiny IEEE input may be over 1000 binary places below the integer bias.
                // Its exact comparison with half a unit suffices; never materialize that gap.
                const int leading=int(value.magnitude.bits())-1+value.exponent2;
                if(leading>=-1) {
                    // A failed alignment of this <=53-bit IEEE source and the
                    // <=64-bit bias proves a huge out-of-range source, not a
                    // representable value near an endpoint. Bias cannot change
                    // its overflow direction; preserve the admitted range policy.
                    auto result=encode<To>(value,out,p);result.loss|=loss;return result;
                }
                if(p.precision_loss==PrecisionLoss::RejectInexact)return {NumericStatus::PrecisionLoss};
                auto result=bias.magnitude.low();
                if(value.negative && p.rounding==Rounding::TowardZero)--result;
                return {NumericStatus::Ok,result,loss|std::uint32_t(LossFlag::Rounded)};
            }
        }
    } else if(p.scaling.mode==ScaleMode::IntegerAffine || p.scaling.mode==ScaleMode::DyadicAffine) {
        auto gain=literal(p.scaling.gain.numerator).value,offset=literal(p.scaling.offset.numerator).value;
        gain.exponent2+=p.scaling.gain.exponent2;offset.exponent2+=p.scaling.offset.exponent2;
        if(!value.magnitude.multiply(gain.magnitude))return {NumericStatus::Overflow};
        value.exponent2+=gain.exponent2;value.negative=value.negative!=gain.negative;
        if(!add_binary(value,offset))return {NumericStatus::Overflow};
    } else if(p.scaling.mode==ScaleMode::Binary64Affine) {
#if DSP_KERNELS_HAS_DECLARED_IEEE64_ENVIRONMENT
        auto gain=literal(p.scaling.gain.numerator).value,offset=literal(p.scaling.offset.numerator).value;
        gain.exponent2+=p.scaling.gain.exponent2;offset.exponent2+=p.scaling.offset.exponent2;
        const auto a=pack_ieee(value,11,52,true,true),b=pack_ieee(gain,11,52,true,true),c=pack_ieee(offset,11,52,true,true);
        if(a.error!=ArithmeticError::Ok || b.error!=ArithmeticError::Ok || c.error!=ArithmeticError::Ok)return {NumericStatus::Overflow};
        NearestEnvironment environment;if(!environment.ready)return {NumericStatus::BackendFailure};
        // Volatile stores force the declared separate IEEE64 multiply and add (no hidden FMA).
        volatile double product_value=std::bit_cast<double>(a.bits)*std::bit_cast<double>(b.bits);
        volatile double affine_value=product_value+std::bit_cast<double>(c.bits);
        const auto decoded=decode_ieee(std::bit_cast<std::uint64_t>(double(affine_value)),11,52);
        if(decoded.special!=Special::Finite)return {NumericStatus::Overflow};
        value=decoded.value;
        if(a.rounded || b.rounded || c.rounded || std::fetestexcept(FE_INEXACT))loss|=std::uint32_t(LossFlag::Rounded);
        if(a.underflow || b.underflow || c.underflow || std::fetestexcept(FE_UNDERFLOW))loss|=std::uint32_t(LossFlag::Underflow);
#else
        // Prepared admission already rejects this specific unavailable mode.
        return {NumericStatus::UnsupportedCombination};
#endif
    }
    auto result=encode<To>(value,out,p);result.loss|=loss;return result;
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
template<DType From,DType To> NumericResult run(const PreparedConversion& plan,const NumericView& input,MutableNumericView& output,std::span<std::byte> scratch) noexcept {
    const auto in=plan.input.contract(),out=plan.output.contract();
    const auto rows=in.shape.rank==Rank::Scalar?1:input.actual[0];
    const auto columns=in.shape.rank==Rank::Matrix?input.actual[1]:1;
    std::size_t output_width=0;storage_width(out,output_width);
    std::size_t n=0;std::uint32_t loss=0;
    for(std::size_t plane=0;plane<input.planes.size();++plane)for(std::size_t row=0;row<rows;++row)for(std::size_t col=0;col<columns;++col) {
        std::uint64_t bits=0;
        if(load_bits(input,{plane,row,col},From,bits)!=Status::ok)return {NumericStatus::InvalidLayout,0,plan.requirements.scratch_bytes,plane,row*columns+col};
        const auto result=element<To>(decode<From>(bits),in,out,plan.policy);
        if(result.status!=NumericStatus::Ok)return {result.status,0,plan.requirements.scratch_bytes,plane,row*columns+col};
        write_encoded<To>(scratch.data()+n*output_width,output_width,result.bits,out);++n;loss|=result.loss;
    }
    n=0;
    // Every operation that can fail precedes this scatter; source/output aliasing is safe after staging.
    for(std::size_t plane=0;plane<output.planes.size();++plane)for(std::size_t row=0;row<rows;++row)for(std::size_t col=0;col<columns;++col) {
        const auto& layout=output.planes[plane];
        const auto physical=output.ring.capacity?(output.ring.oldest+row)%output.ring.capacity:row;
        const auto offset=layout.offset+(out.shape.rank==Rank::Scalar?0:physical*layout.stride_bytes[0])+(out.shape.rank==Rank::Matrix?col*layout.stride_bytes[1]:0);
        std::memcpy(output.storage.data()+offset,scratch.data()+n*output_width,output_width);++n;
    }
    return {NumericStatus::Ok,n*output_width,plan.requirements.scratch_bytes,size_max,size_max,loss};
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
}
ConversionSupport conversion_support(const ConversionRequest& r,const KernelCapabilities& caps) noexcept {
    if(!caps.converter)return {NumericStatus::UnsupportedCombination,{}};
    for(const auto* contract:{&r.input,&r.output}) {
        const auto status=validate_contract(DataContract{*contract},r.frame_limits);
        if(status!=Status::ok)return {status==Status::exhausted || status==Status::overflow?NumericStatus::LimitExceeded:NumericStatus::InvalidContract,{}};
    }
    if(!same_geometry(r.input,r.output))return {NumericStatus::InvalidContract,{}};
    if((r.input.dtype==DType::Float64 || r.output.dtype==DType::Float64) && !caps.float64_arithmetic)return {NumericStatus::UnsupportedCombination,{}};
    const auto policy=policy_status(r);if(policy!=NumericStatus::Ok)return {policy,{}};
    if(r.policy.scaling.mode==ScaleMode::Binary64Affine && !caps.float64_arithmetic)return {NumericStatus::UnsupportedCombination,{}};
    if(r.policy.nonfinite!=NonFinite::Reject) {
        for(const auto* replacement:{&r.policy.nan_replacement,&r.policy.negative_inf_replacement,&r.policy.positive_inf_replacement})
            if(prepare_element(literal(*replacement),r.input,r.output,r.policy).status!=NumericStatus::Ok)return {NumericStatus::InvalidPolicy,{}};
    }
    if(r.input.channel_order.size()>KernelContract::max_planes || r.input.clock.domain.size()>KernelContract::max_clock_bytes ||
       r.input.channel_order.size()>r.limits.max_planes)return {NumericStatus::LimitExceeded,{}};
    std::size_t count=r.input.channel_order.size();
    const unsigned rank=r.input.shape.rank==Rank::Scalar?0:r.input.shape.rank==Rank::Vector?1:2;
    for(unsigned i=0;i<rank;++i)if(!product(count,std::size_t(r.input.shape.extents[i].maximum),count))return {NumericStatus::LimitExceeded,{}};
    if(count>r.limits.max_elements)return {NumericStatus::LimitExceeded,{}};
    std::size_t bytes=0,input_bytes=0,out_width=0,in_width=0;
    storage_width(r.input,in_width);storage_width(r.output,out_width);
    if(!product(count,out_width,bytes) || !product(count,in_width,input_bytes) || bytes>r.limits.max_scratch_bytes ||
       bytes>r.limits.max_output_bytes || input_bytes>r.limits.max_input_bytes)return {NumericStatus::LimitExceeded,{}};
    return {NumericStatus::Ok,{bytes,1,sizeof(PreparedConversion),numeric_reference_stack_bytes}};
}
NumericStatus prepare_conversion(const ConversionRequest& r,const KernelCapabilities& caps,PreparedConversion& out) noexcept {
    const auto support=conversion_support(r,caps);if(support.status!=NumericStatus::Ok)return support.status;
    PreparedConversion plan; snapshot(r.input,plan.input);snapshot(r.output,plan.output);
    plan.policy=r.policy;plan.limits=r.limits;plan.frame_limits=r.frame_limits;plan.requirements=support.requirements;plan.function=select(r.input.dtype,r.output.dtype);
    out=plan;return NumericStatus::Ok;
}
NumericResult convert(const PreparedConversion& plan,const NumericView& input,MutableNumericView& output,std::span<std::byte> scratch) noexcept {
    if(!plan.function)return {NumericStatus::UnsupportedCombination};
    const auto in=plan.input.contract(),out=plan.output.contract();
    if(!input.contract || !output.contract || !same_physical_contract(DataContract{in},DataContract{*input.contract}) ||
       !same_physical_contract(DataContract{out},DataContract{*output.contract}) || input.actual!=output.actual)return {NumericStatus::InvalidContract};
    const auto a=map_status(validate_view(DataFrameView{{},input},plan.frame_limits));if(a!=NumericStatus::Ok)return {a};
    const auto b=map_status(validate_mutable(output,plan.frame_limits));if(b!=NumericStatus::Ok)return {b};
    if(input.storage.size()>plan.limits.max_input_bytes || output.storage.size()>plan.limits.max_output_bytes)return {NumericStatus::LimitExceeded};
    if(scratch.size()<plan.requirements.scratch_bytes)return {NumericStatus::BufferTooSmall,0,plan.requirements.scratch_bytes};
    scratch=scratch.first(plan.requirements.scratch_bytes);
    auto conflict=[&](std::span<const std::byte> metadata) noexcept {return overlap(scratch,metadata) || overlap(output.storage,metadata);};
    if(overlap(scratch,input.storage) || overlap(scratch,output.storage) || conflict(std::as_bytes(std::span(&plan,1))) ||
       conflict(std::as_bytes(std::span(&input,1))) || conflict(std::as_bytes(std::span(&output,1))) ||
       conflict(std::as_bytes(std::span(input.contract,1))) || conflict(std::as_bytes(std::span(output.contract,1))) ||
       conflict(std::as_bytes(input.planes)) || conflict(std::as_bytes(output.planes)) ||
       conflict(std::as_bytes(input.contract->channel_order)) || conflict(std::as_bytes(output.contract->channel_order)) ||
       conflict(std::as_bytes(std::span(input.contract->clock.domain.data(),input.contract->clock.domain.size()))) ||
       conflict(std::as_bytes(std::span(output.contract->clock.domain.data(),output.contract->clock.domain.size()))))return {NumericStatus::InvalidLayout};
    if(conflict(input.ring.silence_mask) || conflict(output.ring.silence_mask))return {NumericStatus::InvalidLayout};
    for(unsigned i=0;i<2;++i)if(conflict(input.coordinates.axes[i]) || conflict(output.coordinates.axes[i]))return {NumericStatus::InvalidLayout};
    for(unsigned i=0;i<2;++i)if(input.coordinates.axes[i].size()!=output.coordinates.axes[i].size() ||
       !std::equal(input.coordinates.axes[i].begin(),input.coordinates.axes[i].end(),output.coordinates.axes[i].begin()))return {NumericStatus::InvalidContract};
    return plan.function(plan,input,output,scratch);
}
}
