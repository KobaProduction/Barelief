#include "numeric_extension_state.h"
#include "preparation_support.h"
#include <cstddef>

namespace dataflow_runtime::detail {
namespace {
bool add_array(std::size_t& total,std::size_t count,std::size_t width) noexcept {
    std::size_t bytes=0;return numeric_mul(count,width,bytes) && numeric_add(total,bytes);
}
template<class T> bool vector_storage(const std::vector<T>& values,ProcessingResources& resources) noexcept {
    return add_array(resources.plan_bytes,values.capacity(),sizeof(T));
}
bool latch_payload(const NumericInputLatch& latch,ProcessingResources& resources) noexcept {
    if(!vector_storage(latch.buffer.planes,resources) ||
       !add_array(resources.payload_bytes,latch.buffer.floats.capacity(),sizeof(float)) ||
       !numeric_add(resources.payload_bytes,latch.buffer.bytes.capacity()) || !numeric_add(resources.payload_bytes,latch.stream.capacity()))return false;
    for(const auto& axis:latch.buffer.coordinates)if(!numeric_add(resources.payload_bytes,axis.capacity()))return false;
    return true;
}
template<class Extension> bool input_arrays(const Extension& state,ProcessingResources& resources) noexcept {
    return vector_storage(state.input_ports,resources) && vector_storage(state.latches,resources) &&
        vector_storage(state.inputs,resources) && vector_storage(state.raw_inputs,resources) &&
        vector_storage(state.coordinate_groups,resources) && vector_storage(state.input_clocks,resources);
}
template<class Extension> bool output_arrays(const Extension& state,ProcessingResources& resources) noexcept {
    return vector_storage(state.output_ports,resources) && vector_storage(state.outputs,resources) &&
        vector_storage(state.output_contracts,resources) && vector_storage(state.output_buffer_contracts,resources) &&
        vector_storage(state.raw_outputs,resources) && vector_storage(state.coordinate_storage,resources) &&
        vector_storage(state.coordinate_counts,resources) && vector_storage(state.output_streams,resources) &&
        vector_storage(state.output_stream_sizes,resources) && vector_storage(state.output_stamps,resources) &&
        vector_storage(state.output_statuses,resources);
}
template<class T> bool shared_storage(const std::shared_ptr<T>& value,ProcessingResources& resources) noexcept {
    if(!value)return true;
    // Same conservative make_shared control-block/alignment allowance as admission.
    constexpr auto overhead=4*sizeof(void*)+alignof(std::max_align_t);
    return numeric_add(resources.plan_bytes,sizeof(T)+overhead);
}
template<class Extension> bool provenance_storage(const Extension& state,ProcessingResources& resources) noexcept {
    if(!shared_storage(state.authority,resources) || !shared_storage(state.writer_lease,resources))return false;
    for(const auto& clock:state.input_clocks){
        if(!shared_storage(clock,resources))return false;
        if(clock && (!numeric_add(resources.plan_bytes,clock->domain.capacity()) || !numeric_add(resources.plan_bytes,1)))return false;
    }
    return true;
}
bool private_contracts(std::span<const ProcessingPort> ports,ProcessingResources& resources) noexcept {
    for(const auto& port:ports)if(!numeric_add(resources.plan_bytes,port.contract.owned_bytes()))return false;
    return true;
}
}
bool PreparedNumericExtension::Impl::owned_resources(ProcessingResources& resources) const noexcept {
    ProcessingResources next;next.plan_bytes=sizeof(Impl);next.stack_bytes=4096;
    if(!input_arrays(*this,next) || !output_arrays(*this,next) || !provenance_storage(*this,next) ||
       !private_contracts(input_ports,next) || !private_contracts(output_ports,next))return false;
    for(const auto& latch:latches)if(!latch_payload(latch,next))return false;
    // Output coordinate spans alias State buffers, accounted by their owner.
    for(const auto& stream:output_streams)if(!numeric_add(next.payload_bytes,stream.capacity()))return false;
    resources=next;return true;
}
} // namespace dataflow_runtime::detail
