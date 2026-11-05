// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_buffers.h"
#include "frame_operation_io.h"
#include <functional>

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
namespace {
bool overlaps(std::span<const std::byte> a,std::span<const std::byte> b) noexcept {
    if(a.empty() || b.empty())return false;
    const std::less<const std::byte*> before;
    return before(a.data(),b.data()+b.size()) && before(b.data(),a.data()+a.size());
}
bool disjoint_inputs(std::span<const std::byte> buffer,std::span<const NumericOperationInput> inputs) noexcept {
    for(const auto& input:inputs){
        if(overlaps(buffer,input.value.storage) || overlaps(buffer,std::as_bytes(input.contiguous_float32)))return false;
        for(const auto coordinates:input.value.coordinates.axes)if(overlaps(buffer,coordinates))return false;
    }
    return true;
}
bool disjoint_outputs(std::span<const std::byte> buffer,std::span<const NumericOperationOutput> outputs,std::size_t owner,std::size_t axis) noexcept {
    for(std::size_t i=0;i<outputs.size();++i){
        if(overlaps(buffer,outputs[i].value.storage))return false;
        for(std::size_t j=0;j<2;++j)
            if((i!=owner || j!=axis) && overlaps(buffer,outputs[i].writable_coordinates[j]))return false;
    }
    return true;
}
}
bool pcm_stage_buffers_disjoint(std::span<const NumericOperationInput> inputs,std::span<const NumericOperationOutput> outputs) noexcept {
    if(!disjoint_operation_buffers(inputs,outputs) || !proof_buffers_disjoint(inputs,outputs))return false;
    for(std::size_t i=0;i<outputs.size();++i){
        if(!disjoint_inputs(outputs[i].value.storage,inputs))return false;
        for(std::size_t axis=0;axis<2;++axis){
            const auto buffer=outputs[i].writable_coordinates[axis];
            if(!disjoint_inputs(buffer,inputs) || !disjoint_outputs(buffer,outputs,i,axis))return false;
        }
    }
    return true;
}
}
