#include "numeric_extension_state.h"
#include "preparation_support.h"

namespace dataflow_runtime::detail {
namespace {
bool latch_resources(const NumericContract& contract,const NumericRuntimeProfile& profile,ProcessingResources& resources) noexcept {
    std::size_t bytes=0,stride=0,plane=0,layouts=0;std::array<std::size_t,2> shape;
    if(!numeric_geometry(contract,bytes,stride,plane,shape) ||
       !numeric_mul(contract.channel_order.size(),sizeof(PlaneLayout),layouts) ||
       !numeric_add(resources.payload_bytes,bytes) || !numeric_add(resources.payload_bytes,profile.admission.frame.max_string_bytes) ||
       !numeric_add(resources.plan_bytes,layouts))return false;
    for(std::size_t axis=0;axis<2;++axis)if(contract.coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE){
        if(!numeric_mul(std::size_t(contract.shape.extents[axis].maximum),sizeof(double)*(axis==0 && contract.semantic==Semantic::History?2:1),bytes) || !numeric_add(resources.payload_bytes,bytes))return false;
    }
    return true;
}
bool wrapper_arrays(const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile,ProcessingResources& resources,std::size_t state_bytes) noexcept {
    std::size_t bytes=0;
    if(!numeric_add(resources.plan_bytes,state_bytes) ||
       !numeric_mul(source.inputs.size(),sizeof(NumericInputLatch)+sizeof(NumericOperationInput),bytes) || !numeric_add(resources.plan_bytes,bytes) ||
       !numeric_mul(source.outputs.size(),sizeof(NumericOperationOutput)+sizeof(NumericContract)+sizeof(std::vector<char>)+sizeof(std::size_t)+sizeof(FrameStamp)+sizeof(Status),bytes) || !numeric_add(resources.plan_bytes,bytes) ||
       !numeric_mul(source.outputs.size(),profile.admission.frame.max_string_bytes,bytes))return false;
    return numeric_add(resources.payload_bytes,bytes) && numeric_add(resources.stack_bytes,4096);
}
bool add_resources(ProcessingResources& total,const ProcessingResources& value) noexcept {
    return numeric_add(total.plan_bytes,value.plan_bytes) && numeric_add(total.payload_bytes,value.payload_bytes) &&
        numeric_add(total.scratch_bytes,value.scratch_bytes) && numeric_add(total.stack_bytes,value.stack_bytes);
}
ProcessingSupport assess_extension(const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile) {
    if(!profile.operation_factory)return {};
    auto result=profile.operation_factory->assess(source,profile);ProcessingResources wrapper;
    if(!result.supported || !extension_owned_resources(source,profile,wrapper) || !add_resources(result.resources,wrapper))return {};
    return result;
}
}
bool extension_owned_resources(const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile,ProcessingResources& resources) noexcept {
    if(!wrapper_arrays(source,profile,resources,sizeof(PreparedNumericExtension::Impl)))return false;
    for(const auto& port:source.inputs){
        const auto value=port.contract.view();const auto* contract=std::get_if<NumericContract>(&value);
        if(!contract || !latch_resources(*contract,profile,resources))return false;
    }
    return true;
}
ProcessingSupport PreparedNumericExtension::assess(const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile) noexcept {
#if defined(__cpp_exceptions)
    try{return assess_extension(source,profile);}catch(...){return {};}
#else
    return assess_extension(source,profile);
#endif
}
} // namespace dataflow_runtime::detail
