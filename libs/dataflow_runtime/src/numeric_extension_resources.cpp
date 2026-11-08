#include "numeric_extension_state.h"
#include "preparation_support.h"
#include <algorithm>
#include <cstddef>

namespace dataflow_runtime::detail {
namespace {
bool add_array(std::size_t& total,std::size_t count,std::size_t width) noexcept {
    std::size_t bytes=0;return numeric_mul(count,width,bytes) && numeric_add(total,bytes);
}
bool latch_resources(const NumericContract& contract,const NumericRuntimeProfile& profile,ProcessingResources& resources) noexcept {
    std::size_t bytes=0,stride=0,plane=0;std::array<std::size_t,2> shape;
    if(!numeric_geometry(contract,bytes,stride,plane,shape) ||
       !add_array(resources.plan_bytes,contract.channel_order.size(),sizeof(PlaneLayout)) ||
       !numeric_add(resources.payload_bytes,bytes) || !numeric_add(resources.payload_bytes,profile.admission.frame.max_string_bytes))return false;
    for(std::size_t axis=0;axis<2;++axis)if(contract.coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE){
        const auto width=sizeof(double)*(axis==0 && contract.semantic==Semantic::History?2:1);
        if(!add_array(resources.payload_bytes,std::size_t(contract.shape.extents[axis].maximum),width))return false;
    }
    return true;
}
bool input_arrays(std::size_t count,ProcessingResources& resources) noexcept {
    constexpr auto width=sizeof(ProcessingPort)+sizeof(NumericInputLatch)+sizeof(NumericOperationInput)+
        sizeof(DataFrameView)+sizeof(std::size_t)+sizeof(std::shared_ptr<const NumericOperationClock>);
    return add_array(resources.plan_bytes,count,width);
}
bool output_arrays(std::size_t count,const NumericRuntimeProfile& profile,ProcessingResources& resources) noexcept {
    constexpr auto width=sizeof(ProcessingPort)+sizeof(NumericOperationOutput)+sizeof(NumericContract)+sizeof(NumericBufferContract)+
        sizeof(MutableNumericView)+sizeof(std::array<std::span<std::byte>,2>)+sizeof(std::array<std::size_t,2>)+
        sizeof(std::vector<char>)+sizeof(std::size_t)+sizeof(FrameStamp)+sizeof(Status);
    return add_array(resources.plan_bytes,count,width) &&
        add_array(resources.payload_bytes,count,profile.admission.frame.max_string_bytes);
}
bool shared_state(ProcessingResources& resources) noexcept {
    // Conservative make_shared control-block/alignment allowance per object.
    constexpr auto overhead=4*sizeof(void*)+alignof(std::max_align_t);
    return numeric_add(resources.plan_bytes,sizeof(NumericOperationAuthority)+overhead) &&
        numeric_add(resources.plan_bytes,sizeof(NumericOperationWriterLease)+overhead);
}
bool clock_resources(const ClockContract& clock,ProcessingResources& resources) noexcept {
    constexpr auto overhead=4*sizeof(void*)+alignof(std::max_align_t);
    std::size_t text=0;
    if(!numeric_mul(clock.domain.size(),2,text))return false;
    // Covers SSO and the constructor's rounded string capacity, including NUL.
    text=std::max(text,sizeof(std::string));
    return numeric_add(text,1) && numeric_add(resources.plan_bytes,sizeof(NumericOperationClock)+overhead) &&
        numeric_add(resources.plan_bytes,text);
}
bool private_contracts(std::span<const ProcessingPort> ports,ProcessingResources& resources) noexcept {
    // Full views borrow these pinned owners; do not charge their metadata again.
    for(const auto& port:ports)if(!numeric_add(resources.plan_bytes,port.contract.owned_bytes()))return false;
    return true;
}
bool add_resources(ProcessingResources& total,const ProcessingResources& value) noexcept {
    return numeric_add(total.plan_bytes,value.plan_bytes) && numeric_add(total.payload_bytes,value.payload_bytes) &&
        numeric_add(total.scratch_bytes,value.scratch_bytes) && numeric_add(total.stack_bytes,value.stack_bytes);
}
ProcessingSupport assess_extension(const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile) {
    if(!profile.operation_factory)return {};
    auto result=profile.operation_factory->assess(numeric_operation_node_view(source),numeric_operation_profile(profile));
    ProcessingResources wrapper;
    if(!result.supported || !extension_owned_resources(source,profile,wrapper) || !add_resources(result.resources,wrapper))return {};
    return result;
}
}
bool extension_owned_resources(const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile,ProcessingResources& resources) noexcept {
    ProcessingResources next;next.plan_bytes=sizeof(PreparedNumericExtension::Impl);next.stack_bytes=4096;
    if(!input_arrays(source.inputs.size(),next) || !output_arrays(source.outputs.size(),profile,next) || !shared_state(next) ||
       !private_contracts(source.inputs,next) || !private_contracts(source.outputs,next))return false;
    for(const auto& port:source.inputs){
        const auto value=port.contract.view();const auto* contract=std::get_if<NumericContract>(&value);
        if(!contract || !latch_resources(*contract,profile,next) || !clock_resources(contract->clock,next))return false;
    }
    resources=next;return true;
}
ProcessingSupport PreparedNumericExtension::assess(const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile) noexcept {
#if defined(__cpp_exceptions)
    try{return assess_extension(source,profile);}catch(...){return {};}
#else
    return assess_extension(source,profile);
#endif
}
} // namespace dataflow_runtime::detail
