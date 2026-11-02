#include "numeric_extension_state.h"
#include "preparation_support.h"

namespace dataflow_runtime::detail {
namespace {
bool add_array(std::size_t& total,std::size_t count,std::size_t width) noexcept {
    std::size_t bytes=0;return numeric_mul(count,width,bytes) && numeric_add(total,bytes);
}
bool latch_payload(const NumericInputLatch& latch,ProcessingResources& resources) noexcept {
    if(!add_array(resources.plan_bytes,latch.buffer.planes.capacity(),sizeof(PlaneLayout)) ||
       !add_array(resources.payload_bytes,latch.buffer.floats.capacity(),sizeof(float)) ||
       !numeric_add(resources.payload_bytes,latch.buffer.bytes.capacity()) || !numeric_add(resources.payload_bytes,latch.stream.capacity()))return false;
    for(const auto& axis:latch.buffer.coordinates)if(!numeric_add(resources.payload_bytes,axis.capacity()))return false;
    return true;
}
}
bool PreparedNumericExtension::Impl::owned_resources(ProcessingResources& resources) const noexcept {
    resources.plan_bytes=sizeof(Impl);resources.stack_bytes=4096;
    if(!add_array(resources.plan_bytes,latches.capacity(),sizeof(NumericInputLatch)) || !add_array(resources.plan_bytes,inputs.capacity(),sizeof(NumericOperationInput)) ||
       !add_array(resources.plan_bytes,outputs.capacity(),sizeof(NumericOperationOutput)) || !add_array(resources.plan_bytes,output_contracts.capacity(),sizeof(NumericContract)) ||
       !add_array(resources.plan_bytes,output_streams.capacity(),sizeof(std::vector<char>)) || !add_array(resources.plan_bytes,output_stream_sizes.capacity(),sizeof(std::size_t)) ||
       !add_array(resources.plan_bytes,output_stamps.capacity(),sizeof(FrameStamp)) || !add_array(resources.plan_bytes,output_statuses.capacity(),sizeof(Status)))return false;
    for(const auto& latch:latches)if(!latch_payload(latch,resources))return false;
    for(const auto& stream:output_streams)if(!numeric_add(resources.payload_bytes,stream.capacity()))return false;
    return true;
}
} // namespace dataflow_runtime::detail
