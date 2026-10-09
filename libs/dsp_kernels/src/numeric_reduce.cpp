#include <dsp_kernels/numeric_reduce.h>
#include "numeric_arithmetic.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
namespace dsp_kernels {
namespace {
using namespace signal_contracts;using namespace numeric_detail;
constexpr auto size_max=std::numeric_limits<std::size_t>::max();
bool multiply(std::size_t a,std::size_t b,std::size_t& out) noexcept {if(b && a>size_max/b)return false;out=a*b;return true;}
bool overlap(std::span<const std::byte> a,std::span<const std::byte> b) noexcept {
    if(a.empty() || b.empty())return false;
    const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());return x<=y?y-x<a.size():x-y<b.size();
}
void snapshot(const NumericContract& c,KernelContract& out) noexcept {
    out.value=c;out.plane_count=c.channel_order.size();out.clock_size=c.clock.domain.size();
    std::copy(c.channel_order.begin(),c.channel_order.end(),out.channels.begin());std::copy(c.clock.domain.begin(),c.clock.domain.end(),out.clock.begin());
    out.value.channel_order={};out.value.clock.domain={};
}
template<DType From> NumericResult run(const PreparedReduction& plan,const NumericView& input,MutableNumericView& output,std::span<std::byte> scratch) noexcept {
    constexpr bool ieee=From==DType::Float32 || From==DType::Float64;
    const auto* descriptor=dtype_descriptor(From);
    const auto in=plan.input.contract(),out=plan.output.contract();
    const auto rows=in.shape.rank==Rank::Scalar?1:input.actual[0],columns=in.shape.rank==Rank::Matrix?input.actual[1]:1;
    Binary maximum{},sum{};Magnitude squares{};
    std::uint64_t count=0;
    // Float64 mean/RMS declare scaled sequential IEEE64 evaluation. Float32 uses
    // exact software dyadic aggregates below, independent of native fenv/FPU.
#if DSP_KERNELS_HAS_DECLARED_IEEE64_ENVIRONMENT
    NearestEnvironment environment(From==DType::Float64 && plan.operation!=ScalarReduction::MaximumAbsolute);
    if(!environment.ready)return {NumericStatus::BackendFailure};
#endif
    auto each=[&](auto callback) noexcept -> NumericResult {
        for(std::size_t p=0;p<input.planes.size();++p)for(std::size_t r=0;r<rows;++r)for(std::size_t c=0;c<columns;++c){
            std::uint64_t bits=0;if(load_bits(input,{p,r,c},From,bits)!=Status::ok)return {NumericStatus::InvalidLayout,0,plan.requirements.scratch_bytes,p,r*columns+c};
            const auto decoded=ieee?decode_ieee(bits,From==DType::Float32?8:11,From==DType::Float32?23:52):decode_integer(bits,descriptor->bytes*8,descriptor->signed_value);
            if(decoded.special!=Special::Finite)return {NumericStatus::NonFiniteInput,0,plan.requirements.scratch_bytes,p,r*columns+c};
            if(!callback(decoded.value))return {NumericStatus::Overflow,0,plan.requirements.scratch_bytes,p,r*columns+c};
        }return {NumericStatus::Ok};
    };
    const auto scanned=each([&](Binary value) noexcept {
        ++count;value.negative=false;
        if(absolute_compare(value,maximum)>0)maximum=value;
        return true;
    });if(scanned.status!=NumericStatus::Ok)return scanned;
    if(!count)return {NumericStatus::EmptyPopulation};
    Packed packed{ArithmeticError::Ok,0};
    const bool allow=plan.precision_loss==PrecisionLoss::AllowDeclared,nearest=plan.rounding==Rounding::NearestTiesEven;
    const auto* result_descriptor=dtype_descriptor(out.dtype);
    if(plan.operation==ScalarReduction::MaximumAbsolute) {
        packed=result_descriptor->ieee?pack_ieee(maximum,out.dtype==DType::Float32?8:11,out.dtype==DType::Float32?23:52,nearest,allow):
            pack_integer(maximum,out.representation.valid_bits?out.representation.valid_bits:result_descriptor->bytes*8,result_descriptor->signed_value,nearest,allow);
    } else if constexpr(!ieee || From==DType::Float32) {
        const auto aggregate=each([&](const Binary& value) noexcept {
            if(plan.operation==ScalarReduction::SignedMean)return add_binary(sum,value);
            auto square=value.magnitude;
            if(!square.multiply(value.magnitude))return false;
            if constexpr(From==DType::Float32)if(!square.left(unsigned(2*value.exponent2+298)))return false;
            return squares.add(square);
        });if(aggregate.status!=NumericStatus::Ok)return aggregate;
        packed=pack_ratio(plan.operation==ScalarReduction::SignedMean?sum.magnitude:squares,count,sum.negative,plan.operation==ScalarReduction::Rms,
                          out.dtype==DType::Float32,nearest,allow,
                          plan.operation==ScalarReduction::SignedMean?sum.exponent2:From==DType::Float32?-298:0,From==DType::Float32);
    } else {
#if DSP_KERNELS_HAS_DECLARED_IEEE64_ENVIRONMENT
        const auto scale_packed=pack_ieee(maximum,11,52,true,true);
        const double scale=std::bit_cast<double>(scale_packed.bits);
        volatile double total=0;
        const auto aggregate=each([&](const Binary& value) noexcept {
            const auto x=pack_ieee(value,11,52,true,true);
            volatile double normalized=scale?std::bit_cast<double>(x.bits)/scale:0;
            volatile double contribution=plan.operation==ScalarReduction::Rms?normalized*normalized:normalized;
            total=total+contribution;return true;
        });if(aggregate.status!=NumericStatus::Ok)return aggregate;
        volatile double average=total/double(count);
        volatile double final_value=(plan.operation==ScalarReduction::Rms?std::sqrt(double(average)):double(average))*scale;
        const auto decoded=decode_ieee(std::bit_cast<std::uint64_t>(double(final_value)),11,52);
        if(decoded.special!=Special::Finite)return {NumericStatus::Overflow};
        packed=pack_ieee(decoded.value,out.dtype==DType::Float32?8:11,out.dtype==DType::Float32?23:52,nearest,true);
        packed.rounded=packed.rounded || std::fetestexcept(FE_INEXACT);
        packed.underflow=packed.underflow || std::fetestexcept(FE_UNDERFLOW);
#else
        // Exact MaximumAbsolute and all integer reductions above remain compiled.
        // Prepare refuses floating Mean/RMS before publishing a function pointer.
        return {NumericStatus::UnsupportedCombination};
#endif
    }
    if(packed.error!=ArithmeticError::Ok)return {packed.error==ArithmeticError::Inexact?NumericStatus::PrecisionLoss:NumericStatus::Overflow};
    std::size_t bytes=0;storage_width(out,bytes);
    const unsigned bits=out.representation.valid_bits?out.representation.valid_bits:result_descriptor->bytes*8;
    const auto mask=bits==64?UINT64_MAX:(std::uint64_t{1}<<bits)-1;
    auto raw=packed.bits&mask;
    if(!result_descriptor->ieee && bits<bytes*8) {
        if(out.representation.placement==BitPlacement::MostSignificant)raw<<=unsigned(bytes*8)-bits;
        else if(result_descriptor->signed_value && ((raw>>(bits-1))&1))raw|=(bytes==8?UINT64_MAX:(std::uint64_t{1}<<(bytes*8))-1)&~mask;
    }
    const auto encoding=out.representation.encoding;
    const bool big=encoding==StorageEncoding::FixedBigEndian || encoding==StorageEncoding::PackedSigned24BE || (encoding==StorageEncoding::Native && std::endian::native==std::endian::big);
    for(std::size_t i=0;i<bytes;++i)scratch[i]=std::byte((raw>>(8*(big?bytes-1-i:i)))&255);
    std::memcpy(output.storage.data()+output.planes[0].offset,scratch.data(),bytes);
    return {NumericStatus::Ok,bytes,plan.requirements.scratch_bytes,size_max,size_max,
            (packed.rounded?std::uint32_t(LossFlag::Rounded):0)|(packed.underflow?std::uint32_t(LossFlag::Underflow):0)};
}
PreparedReduction::Function select(DType type) noexcept {
#define D_ENTRY(T) case DType::T:return &run<DType::T>
    switch(type){D_ENTRY(Int8);D_ENTRY(UInt8);D_ENTRY(Int16);D_ENTRY(UInt16);D_ENTRY(Int32);D_ENTRY(UInt32);D_ENTRY(Int64);D_ENTRY(UInt64);D_ENTRY(Float32);D_ENTRY(Float64);}
#undef D_ENTRY
    return nullptr;
}
}
ReductionSupport reduction_support(const ReductionRequest& r,const KernelCapabilities& caps) noexcept {
    if(!caps.reduction)return {NumericStatus::UnsupportedCombination,{}};
    for(const auto* contract:{&r.input,&r.output}) {
        const auto status=validate_contract(DataContract{*contract},r.frame_limits);
        if(status!=Status::ok)return {status==Status::exhausted || status==Status::overflow?NumericStatus::LimitExceeded:NumericStatus::InvalidContract,{}};
    }
    if(r.output.shape.rank!=Rank::Scalar || r.input.unit!=r.output.unit ||
       r.input.quantity!=r.output.quantity || r.input.clock.domain!=r.output.clock.domain || r.input.clock.tick_seconds!=r.output.clock.tick_seconds ||
       r.input.clock.sample_rate!=r.output.clock.sample_rate)return {NumericStatus::InvalidContract,{}};
    if(r.operation!=ScalarReduction::MaximumAbsolute && r.operation!=ScalarReduction::SignedMean && r.operation!=ScalarReduction::Rms)
        return {NumericStatus::InvalidPolicy,{}};
    if((r.precision_loss!=PrecisionLoss::RejectInexact && r.precision_loss!=PrecisionLoss::AllowDeclared) ||
       (r.rounding!=Rounding::TowardZero && r.rounding!=Rounding::NearestTiesEven))return {NumericStatus::InvalidPolicy,{}};
    const auto* to=dtype_descriptor(r.output.dtype);
    if((r.input.dtype==DType::Float64 || r.output.dtype==DType::Float64) && !caps.float64_arithmetic)return {NumericStatus::UnsupportedCombination,{}};
    if(r.operation!=ScalarReduction::MaximumAbsolute && (!to->ieee || (r.input.dtype==DType::Float64 && r.precision_loss!=PrecisionLoss::AllowDeclared)))
        return {NumericStatus::UnsupportedCombination,{}};
    if(r.operation!=ScalarReduction::MaximumAbsolute && r.input.dtype==DType::Float64 && !caps.float64_arithmetic)return {NumericStatus::UnsupportedCombination,{}};
    if(r.operation!=ScalarReduction::MaximumAbsolute && r.input.dtype==DType::Float64 && !declared_ieee64_environment_available)
        return {NumericStatus::UnsupportedCombination,{}};
    if(r.input.channel_order.size()>KernelContract::max_planes || r.input.clock.domain.size()>KernelContract::max_clock_bytes ||
       r.output.clock.domain.size()>KernelContract::max_clock_bytes || r.input.channel_order.size()>r.limits.max_planes)
        return {NumericStatus::LimitExceeded,{}};
    std::size_t count=r.input.channel_order.size();
    const unsigned rank=r.input.shape.rank==Rank::Scalar?0:r.input.shape.rank==Rank::Vector?1:2;
    for(unsigned i=0;i<rank;++i)if(!multiply(count,std::size_t(r.input.shape.extents[i].maximum),count))return {NumericStatus::LimitExceeded,{}};
    if(count>r.limits.max_elements || count>UINT32_MAX)return {NumericStatus::LimitExceeded,{}};
    std::size_t bytes=0,in_width=0,input_bytes=0;storage_width(r.input,in_width);storage_width(r.output,bytes);
    if(!multiply(count,in_width,input_bytes) || input_bytes>r.limits.max_input_bytes || bytes>r.limits.max_output_bytes || bytes>r.limits.max_scratch_bytes)
        return {NumericStatus::LimitExceeded,{}};
    return {NumericStatus::Ok,{bytes,1,sizeof(PreparedReduction),numeric_reference_stack_bytes}};
}
NumericStatus prepare_reduction(const ReductionRequest& r,const KernelCapabilities& caps,PreparedReduction& out) noexcept {
    const auto support=reduction_support(r,caps);if(support.status!=NumericStatus::Ok)return support.status;
    PreparedReduction plan;snapshot(r.input,plan.input);snapshot(r.output,plan.output);plan.operation=r.operation;
    plan.precision_loss=r.precision_loss;plan.rounding=r.rounding;plan.limits=r.limits;plan.frame_limits=r.frame_limits;
    plan.requirements=support.requirements;plan.function=select(r.input.dtype);out=plan;return NumericStatus::Ok;
}
NumericResult reduce(const PreparedReduction& plan,const NumericView& input,MutableNumericView& output,std::span<std::byte> scratch) noexcept {
    if(!plan.function)return {NumericStatus::UnsupportedCombination};
    const auto in=plan.input.contract(),out=plan.output.contract();
    if(!input.contract || !output.contract || !same_physical_contract(DataContract{in},DataContract{*input.contract}) ||
       !same_physical_contract(DataContract{out},DataContract{*output.contract}))return {NumericStatus::InvalidContract};
    for(const auto status:{validate_view(DataFrameView{{},input},plan.frame_limits),validate_mutable(output,plan.frame_limits)}) {
        if(status==Status::buffer_too_small)return {NumericStatus::BufferTooSmall};
        if(status==Status::exhausted || status==Status::overflow)return {NumericStatus::LimitExceeded};
        if(status!=Status::ok)return {NumericStatus::InvalidLayout};
    }
    if(input.storage.size()>plan.limits.max_input_bytes || output.storage.size()>plan.limits.max_output_bytes)return {NumericStatus::LimitExceeded};
    if(scratch.size()<plan.requirements.scratch_bytes)return {NumericStatus::BufferTooSmall,0,plan.requirements.scratch_bytes};
    scratch=scratch.first(plan.requirements.scratch_bytes);
    auto conflict=[&](std::span<const std::byte> metadata) noexcept {return overlap(scratch,metadata) || overlap(output.storage,metadata);};
    if(overlap(scratch,input.storage) || overlap(scratch,output.storage) || conflict(std::as_bytes(std::span(&plan,1))) ||
       conflict(std::as_bytes(std::span(&input,1))) || conflict(std::as_bytes(std::span(&output,1))) ||
       conflict(std::as_bytes(std::span(input.contract,1))) || conflict(std::as_bytes(std::span(output.contract,1))) ||
       conflict(std::as_bytes(input.planes)) || conflict(std::as_bytes(output.planes)) || conflict(std::as_bytes(input.contract->channel_order)) ||
       conflict(std::as_bytes(output.contract->channel_order)) || conflict(std::as_bytes(std::span(input.contract->clock.domain.data(),input.contract->clock.domain.size()))) ||
       conflict(std::as_bytes(std::span(output.contract->clock.domain.data(),output.contract->clock.domain.size()))))return {NumericStatus::InvalidLayout};
    for(unsigned i=0;i<2;++i)if(conflict(input.coordinates.axes[i]) || conflict(output.coordinates.axes[i]))return {NumericStatus::InvalidLayout};
    return plan.function(plan,input,output,scratch);
}
}
