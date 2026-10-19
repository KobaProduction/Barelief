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
template<DType From> class ReductionExecution {
    static constexpr bool ieee=From==DType::Float32 || From==DType::Float64;
    const PreparedReduction& plan;
    const NumericView& input;
    MutableNumericView& output;
    std::span<std::byte> scratch;
    const NumericContract in,out;
    const DTypeDescriptor* descriptor=dtype_descriptor(From);
    const std::size_t rows,columns;
    Binary maximum{},sum{};
    Magnitude squares{};
    std::uint64_t count=0;
    Packed packed{ArithmeticError::Ok,0};
    const bool allow,nearest;
    const DTypeDescriptor* result_descriptor;
template<class Callback> NumericResult scan_element(Callback& callback,std::size_t plane,
                                                     std::size_t row,std::size_t column) noexcept {
        std::uint64_t bits=0;
        if(load_bits(input,{plane,row,column},From,bits)!=Status::ok)
            return {NumericStatus::InvalidLayout,0,plan.requirements.scratch_bytes,plane,row*columns+column};
        const auto decoded=ieee?
            decode_ieee(bits,From==DType::Float32?8:11,From==DType::Float32?23:52):
            decode_integer(bits,descriptor->bytes*8,descriptor->signed_value);
        if(decoded.special!=Special::Finite)
            return {NumericStatus::NonFiniteInput,0,plan.requirements.scratch_bytes,plane,row*columns+column};
        if(!callback(decoded.value))
            return {NumericStatus::Overflow,0,plan.requirements.scratch_bytes,plane,row*columns+column};
        return {NumericStatus::Ok};
    }

    template<class Callback> NumericResult scan(Callback callback) noexcept {
        for(std::size_t plane=0;plane<input.planes.size();++plane)
            for(std::size_t row=0;row<rows;++row)
                for(std::size_t column=0;column<columns;++column) {
                    const auto result=scan_element(callback,plane,row,column);
                    if(result.status!=NumericStatus::Ok)return result;
                }
        return {NumericStatus::Ok};
    }
    NumericResult scan_maximum() noexcept {
        return scan([&](Binary value) noexcept {
            ++count;value.negative=false;
            if(absolute_compare(value,maximum)>0)maximum=value;
            return true;
        });
    }
    void pack_maximum() noexcept {
        packed=result_descriptor->ieee?
            pack_ieee(maximum,out.dtype==DType::Float32?8:11,out.dtype==DType::Float32?23:52,nearest,allow):
            pack_integer(maximum,out.representation.valid_bits?
                out.representation.valid_bits:result_descriptor->bytes*8,
                result_descriptor->signed_value,nearest,allow);
    }
    NumericResult pack_exact() noexcept {
        const auto aggregate=scan([&](const Binary& value) noexcept {
            if(plan.operation==ScalarReduction::SignedMean)return add_binary(sum,value);
            auto square=value.magnitude;
            if(!square.multiply(value.magnitude))return false;
            if constexpr(From==DType::Float32)
                if(!square.left(unsigned(2*value.exponent2+298)))return false;
            return squares.add(square);
        });
        if(aggregate.status!=NumericStatus::Ok)return aggregate;
        packed=pack_ratio(plan.operation==ScalarReduction::SignedMean?sum.magnitude:squares,count,
                          sum.negative,plan.operation==ScalarReduction::Rms,
                          out.dtype==DType::Float32,nearest,allow,
                          plan.operation==ScalarReduction::SignedMean?sum.exponent2:
                              From==DType::Float32?-298:0,From==DType::Float32);
        return {NumericStatus::Ok};
    }
NumericResult accumulate_ieee64(double scale,volatile double& total) noexcept {
        return scan([&](const Binary& value) noexcept {
            const auto converted=pack_ieee(value,11,52,true,true);
            volatile double normalized=scale?std::bit_cast<double>(converted.bits)/scale:0;
            volatile double contribution=plan.operation==ScalarReduction::Rms?
                normalized*normalized:normalized;
            total=total+contribution;
            return true;
        });
    }

    NumericResult pack_ieee64() noexcept {
#if DSP_KERNELS_HAS_DECLARED_IEEE64_ENVIRONMENT
        const auto scale_packed=pack_ieee(maximum,11,52,true,true);
        const double scale=std::bit_cast<double>(scale_packed.bits);
        volatile double total=0;
        const auto aggregate=accumulate_ieee64(scale,total);
        if(aggregate.status!=NumericStatus::Ok)return aggregate;
        volatile double average=total/double(count);
        volatile double final_value=(plan.operation==ScalarReduction::Rms?
            std::sqrt(double(average)):double(average))*scale;
        const auto decoded=decode_ieee(std::bit_cast<std::uint64_t>(double(final_value)),11,52);
        if(decoded.special!=Special::Finite)return {NumericStatus::Overflow};
        packed=pack_ieee(decoded.value,out.dtype==DType::Float32?8:11,
                         out.dtype==DType::Float32?23:52,nearest,true);
        packed.rounded=packed.rounded || std::fetestexcept(FE_INEXACT);
        packed.underflow=packed.underflow || std::fetestexcept(FE_UNDERFLOW);
        return {NumericStatus::Ok};
#else
        return {NumericStatus::UnsupportedCombination};
#endif
    }
    NumericResult pack_aggregate() noexcept {
        if constexpr(!ieee || From==DType::Float32)return pack_exact();
        else return pack_ieee64();
    }
    std::uint64_t encoded_bits(std::size_t bytes) const noexcept {
        const unsigned bits=out.representation.valid_bits?
            out.representation.valid_bits:result_descriptor->bytes*8;
        const auto mask=bits==64?UINT64_MAX:(std::uint64_t{1}<<bits)-1;
        auto raw=packed.bits&mask;
        if(!result_descriptor->ieee && bits<bytes*8) {
            if(out.representation.placement==BitPlacement::MostSignificant)
                raw<<=unsigned(bytes*8)-bits;
            else if(result_descriptor->signed_value && ((raw>>(bits-1))&1))
                raw|=(bytes==8?UINT64_MAX:(std::uint64_t{1}<<(bytes*8))-1)&~mask;
        }
        return raw;
    }
    NumericResult publish() noexcept {
        if(packed.error!=ArithmeticError::Ok)
            return {packed.error==ArithmeticError::Inexact?
                NumericStatus::PrecisionLoss:NumericStatus::Overflow};
        std::size_t bytes=0;storage_width(out,bytes);
        const auto raw=encoded_bits(bytes);
        const auto encoding=out.representation.encoding;
        const bool big=encoding==StorageEncoding::FixedBigEndian ||
            encoding==StorageEncoding::PackedSigned24BE ||
            (encoding==StorageEncoding::Native && std::endian::native==std::endian::big);
        for(std::size_t index=0;index<bytes;++index)
            scratch[index]=std::byte((raw>>(8*(big?bytes-1-index:index)))&255);
        std::memcpy(output.storage.data()+output.planes[0].offset,scratch.data(),bytes);
        return {NumericStatus::Ok,bytes,plan.requirements.scratch_bytes,size_max,size_max,
                (packed.rounded?std::uint32_t(LossFlag::Rounded):0)|
                (packed.underflow?std::uint32_t(LossFlag::Underflow):0)};
    }
public:
    ReductionExecution(const PreparedReduction& prepared,const NumericView& source,
                       MutableNumericView& destination,std::span<std::byte> workspace) noexcept
        :plan(prepared),input(source),output(destination),scratch(workspace),
         in(plan.input.contract()),out(plan.output.contract()),
         rows(in.shape.rank==Rank::Scalar?1:input.actual[0]),
         columns(in.shape.rank==Rank::Matrix?input.actual[1]:1),
         allow(plan.precision_loss==PrecisionLoss::AllowDeclared),
         nearest(plan.rounding==Rounding::NearestTiesEven),
         result_descriptor(dtype_descriptor(out.dtype)){}
    NumericResult run() noexcept {
#if DSP_KERNELS_HAS_DECLARED_IEEE64_ENVIRONMENT
        NearestEnvironment environment(From==DType::Float64 &&
            plan.operation!=ScalarReduction::MaximumAbsolute);
        if(!environment.ready)return {NumericStatus::BackendFailure};
#endif
        const auto scanned=scan_maximum();if(scanned.status!=NumericStatus::Ok)return scanned;
        if(!count)return {NumericStatus::EmptyPopulation};
        if(plan.operation==ScalarReduction::MaximumAbsolute)pack_maximum();
        else {
            const auto aggregate=pack_aggregate();
            if(aggregate.status!=NumericStatus::Ok)return aggregate;
        }
        return publish();
    }
};
template<DType From> NumericResult run(const PreparedReduction& plan,const NumericView& input,MutableNumericView& output,std::span<std::byte> scratch) noexcept {
    return ReductionExecution<From>(plan,input,output,scratch).run();
}
PreparedReduction::Function select(DType type) noexcept {
#define D_ENTRY(T) case DType::T:return &run<DType::T>
    switch(type){D_ENTRY(Int8);D_ENTRY(UInt8);D_ENTRY(Int16);D_ENTRY(UInt16);D_ENTRY(Int32);D_ENTRY(UInt32);D_ENTRY(Int64);D_ENTRY(UInt64);D_ENTRY(Float32);D_ENTRY(Float64);}
#undef D_ENTRY
    return nullptr;
}
NumericStatus reduction_contract_status(const ReductionRequest& request,const KernelCapabilities& caps) noexcept {
    if(!caps.reduction)return NumericStatus::UnsupportedCombination;
    for(const auto* contract:{&request.input,&request.output}) {
        const auto status=validate_contract(DataContract{*contract},request.frame_limits);
        if(status!=Status::ok)return status==Status::exhausted || status==Status::overflow?
            NumericStatus::LimitExceeded:NumericStatus::InvalidContract;
    }
    if(request.output.shape.rank!=Rank::Scalar || request.input.unit!=request.output.unit ||
       request.input.quantity!=request.output.quantity ||
       request.input.clock.domain!=request.output.clock.domain ||
       request.input.clock.tick_seconds!=request.output.clock.tick_seconds ||
       request.input.clock.sample_rate!=request.output.clock.sample_rate)return NumericStatus::InvalidContract;
    return NumericStatus::Ok;
}
bool reduction_policy_enums_valid(const ReductionRequest& request) noexcept {
    if(request.operation!=ScalarReduction::MaximumAbsolute &&
       request.operation!=ScalarReduction::SignedMean &&
       request.operation!=ScalarReduction::Rms)return false;
    if(request.precision_loss!=PrecisionLoss::RejectInexact &&
       request.precision_loss!=PrecisionLoss::AllowDeclared)return false;
    return request.rounding==Rounding::TowardZero || request.rounding==Rounding::NearestTiesEven;
}

NumericStatus reduction_policy_status(const ReductionRequest& request,const KernelCapabilities& caps) noexcept {
    if(!reduction_policy_enums_valid(request))return NumericStatus::InvalidPolicy;
    const auto* output=dtype_descriptor(request.output.dtype);
    if((request.input.dtype==DType::Float64 || request.output.dtype==DType::Float64) &&
       !caps.float64_arithmetic)return NumericStatus::UnsupportedCombination;
    if(request.operation!=ScalarReduction::MaximumAbsolute &&
       (!output->ieee || (request.input.dtype==DType::Float64 &&
                          request.precision_loss!=PrecisionLoss::AllowDeclared)))
        return NumericStatus::UnsupportedCombination;
    if(request.operation!=ScalarReduction::MaximumAbsolute && request.input.dtype==DType::Float64 &&
       !caps.float64_arithmetic)return NumericStatus::UnsupportedCombination;
    if(request.operation!=ScalarReduction::MaximumAbsolute && request.input.dtype==DType::Float64 &&
       !declared_ieee64_environment_available)return NumericStatus::UnsupportedCombination;
    return NumericStatus::Ok;
}
bool reduction_element_count(const NumericContract& input,std::size_t& count) noexcept {
    count=input.channel_order.size();
    const unsigned rank=input.shape.rank==Rank::Scalar?0:
        input.shape.rank==Rank::Vector?1:2;
    for(unsigned axis=0;axis<rank;++axis)
        if(!multiply(count,std::size_t(input.shape.extents[axis].maximum),count))return false;
    return true;
}

ReductionSupport reduction_resources(const ReductionRequest& request) noexcept {
    if(request.input.channel_order.size()>KernelContract::max_planes ||
       request.input.clock.domain.size()>KernelContract::max_clock_bytes ||
       request.output.clock.domain.size()>KernelContract::max_clock_bytes ||
       request.input.channel_order.size()>request.limits.max_planes)return {NumericStatus::LimitExceeded,{}};
    std::size_t count=0;
    if(!reduction_element_count(request.input,count))return {NumericStatus::LimitExceeded,{}};
    if(count>request.limits.max_elements || count>UINT32_MAX)return {NumericStatus::LimitExceeded,{}};
    std::size_t bytes=0,input_width=0,input_bytes=0;
    storage_width(request.input,input_width);storage_width(request.output,bytes);
    if(!multiply(count,input_width,input_bytes) || input_bytes>request.limits.max_input_bytes ||
       bytes>request.limits.max_output_bytes || bytes>request.limits.max_scratch_bytes)
        return {NumericStatus::LimitExceeded,{}};
    return {NumericStatus::Ok,{bytes,1,sizeof(PreparedReduction),numeric_reference_stack_bytes}};
}
NumericResult validate_reduction_layouts(const PreparedReduction& plan,const NumericView& input,
                                         const MutableNumericView& output) noexcept {
    for(const auto status:{validate_view(DataFrameView{{},input},plan.frame_limits),
                           validate_mutable(output,plan.frame_limits)}) {
        if(status==Status::buffer_too_small)return {NumericStatus::BufferTooSmall};
        if(status==Status::exhausted || status==Status::overflow)return {NumericStatus::LimitExceeded};
        if(status!=Status::ok)return {NumericStatus::InvalidLayout};
    }
    return {NumericStatus::Ok};
}

NumericResult validate_reduction_views(const PreparedReduction& plan,const NumericView& input,
                                       const MutableNumericView& output,std::span<const std::byte> scratch) noexcept {
    const auto in=plan.input.contract(),out=plan.output.contract();
    if(!input.contract || !output.contract ||
       !same_physical_contract(DataContract{in},DataContract{*input.contract}) ||
       !same_physical_contract(DataContract{out},DataContract{*output.contract}))
        return {NumericStatus::InvalidContract};
    const auto layout=validate_reduction_layouts(plan,input,output);
    if(layout.status!=NumericStatus::Ok)return layout;
    if(input.storage.size()>plan.limits.max_input_bytes ||
       output.storage.size()>plan.limits.max_output_bytes)return {NumericStatus::LimitExceeded};
    if(scratch.size()<plan.requirements.scratch_bytes)
        return {NumericStatus::BufferTooSmall,0,plan.requirements.scratch_bytes};
    return {NumericStatus::Ok};
}
bool reduction_metadata_alias(const PreparedReduction& plan,const NumericView& input,
                              const MutableNumericView& output,std::span<const std::byte> scratch) noexcept {
    const auto conflict=[&](std::span<const std::byte> metadata) noexcept {
        return overlap(scratch,metadata) || overlap(output.storage,metadata);
    };
    if(overlap(scratch,input.storage) || overlap(scratch,output.storage) ||
       conflict(std::as_bytes(std::span(&plan,1))) ||
       conflict(std::as_bytes(std::span(&input,1))) ||
       conflict(std::as_bytes(std::span(&output,1))) ||
       conflict(std::as_bytes(std::span(input.contract,1))) ||
       conflict(std::as_bytes(std::span(output.contract,1))) ||
       conflict(std::as_bytes(input.planes)) || conflict(std::as_bytes(output.planes)) ||
       conflict(std::as_bytes(input.contract->channel_order)) ||
       conflict(std::as_bytes(output.contract->channel_order)) ||
       conflict(std::as_bytes(std::span(input.contract->clock.domain.data(),input.contract->clock.domain.size()))) ||
       conflict(std::as_bytes(std::span(output.contract->clock.domain.data(),output.contract->clock.domain.size()))))
        return true;
    for(unsigned axis=0;axis<2;++axis)
        if(conflict(input.coordinates.axes[axis]) || conflict(output.coordinates.axes[axis]))return true;
    return false;
}
}
ReductionSupport reduction_support(const ReductionRequest& r,const KernelCapabilities& caps) noexcept {
    const auto contracts=reduction_contract_status(r,caps);if(contracts!=NumericStatus::Ok)return {contracts,{}};
    const auto policy=reduction_policy_status(r,caps);if(policy!=NumericStatus::Ok)return {policy,{}};
    return reduction_resources(r);
}
NumericStatus prepare_reduction(const ReductionRequest& r,const KernelCapabilities& caps,PreparedReduction& out) noexcept {
    const auto support=reduction_support(r,caps);if(support.status!=NumericStatus::Ok)return support.status;
    PreparedReduction plan;snapshot(r.input,plan.input);snapshot(r.output,plan.output);plan.operation=r.operation;
    plan.precision_loss=r.precision_loss;plan.rounding=r.rounding;plan.limits=r.limits;plan.frame_limits=r.frame_limits;
    plan.requirements=support.requirements;plan.function=select(r.input.dtype);out=plan;return NumericStatus::Ok;
}
NumericResult reduce(const PreparedReduction& plan,const NumericView& input,MutableNumericView& output,std::span<std::byte> scratch) noexcept {
    if(!plan.function)return {NumericStatus::UnsupportedCombination};
    const auto validation=validate_reduction_views(plan,input,output,scratch);
    if(validation.status!=NumericStatus::Ok)return validation;
    scratch=scratch.first(plan.requirements.scratch_bytes);
    if(reduction_metadata_alias(plan,input,output,scratch))return {NumericStatus::InvalidLayout};
    return plan.function(plan,input,output,scratch);
}
}
