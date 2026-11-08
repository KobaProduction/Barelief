// SPDX-License-Identifier: AGPL-3.0-or-later
#include "frame_operation_io.h"
#include <algorithm>
#include <functional>

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
bool numeric_port(const dataflow_runtime::NumericOperationNodeView& node,bool input,domain::PortId id,NumericBufferContract& contract) noexcept {
    for(const auto& port:input?node.inputs:node.outputs){
        if(port.id!=id)continue;
        contract=port.contract;return true;
    }
    return false;
}

bool equal_frame_identity(const NumericOperationInput& a,const NumericOperationInput& b) noexcept {
    return numeric_operation_same_step(a.origin,b.origin) && numeric_operation_same_coordinates(a.origin,b.origin);
}
bool proof_buffers_disjoint(std::span<const NumericOperationInput> inputs,std::span<const NumericOperationOutput> outputs) noexcept {
    const std::less<const std::byte*> before;
    for(const auto& input:inputs){
        const auto bytes=std::as_bytes(input.contiguous_float32);if(bytes.empty())continue;
        for(const auto& output:outputs){
            const auto storage=output.value.storage;if(storage.empty())continue;
            if(before(bytes.data(),storage.data()+storage.size()) && before(storage.data(),bytes.data()+bytes.size()))return false;
        }
    }
    return true;
}
Status validate_frame_vector(const NumericOperationInput& input,const dataflow_runtime::NumericOperationNodeView& node,
    std::size_t bins,const FrameLimits& limits,bool nonnegative) noexcept {
    NumericBufferContract expected;if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
    const auto status=validate_vector(input,bins,expected,limits);if(status!=Status::ok)return status;
    if(input.contiguous_float32.size()!=bins)return Status::invalid_argument;
    if(nonnegative && std::any_of(input.contiguous_float32.begin(),input.contiguous_float32.end(),[](float x){return x<0;}))return Status::invalid_argument;
    return Status::ok;
}
Status validate_operation_control(const NumericOperationInput& input,const NumericBufferContract& expected,const FrameLimits& limits) noexcept {
    if(input.status!=Status::ok)return Status::ok;
    if(!input.value.contract || !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    const auto status=validate_view(input.value,limits);if(status!=Status::ok)return status;
    std::uint8_t value=0;const auto loaded=load_element(input.value,{0,0,0},value);
    return loaded==Status::ok && value>3?Status::invalid_argument:loaded;
}
dataflow_runtime::NumericOperationOrigin operation_point_stamp(const NumericOperationInput* input) noexcept {
    return input?numeric_operation_origin(input->origin,NumericOperationPublication::Point):NumericOperationOrigin{};
}
dataflow_runtime::NumericOperationOrigin operation_frame_stamp(const NumericOperationInput* input,bool enabled) noexcept {
    if(!input)return {};
    return numeric_operation_origin(input->origin,enabled?NumericOperationPublication::Frame:NumericOperationPublication::DisabledFrame);
}
}
