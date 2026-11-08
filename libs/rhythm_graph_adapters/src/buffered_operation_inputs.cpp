// SPDX-License-Identifier: AGPL-3.0-or-later
#include "buffered_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <algorithm>

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
bool corresponding_bands(const NumericOperationInput& current,const NumericOperationInput& delayed) noexcept {
    return numeric_operation_same_session(current.origin,delayed.origin) && numeric_operation_same_coordinates(current.origin,delayed.origin);
}
Status bound_buffered_input(std::span<const NumericOperationInput> inputs,std::size_t index,const dataflow_runtime::NumericOperationNodeView& node,
    const BufferedPlan& plan,const FrameLimits& limits) noexcept {
    const auto& input=inputs[index];NumericBufferContract expected;
    if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
    if(!operation_input_unique(inputs,index))return Status::invalid_argument;
    return input.port==buffered_control_port?validate_operation_control(input,expected,limits):validate_vector(input,plan.maximum,expected,limits);
}
Status read_buffers(std::span<const NumericOperationInput> inputs,const dataflow_runtime::NumericOperationNodeView& node,
    const BufferedPlan& plan,const FrameLimits& limits) noexcept {
    for(std::size_t i=0;i<inputs.size();++i){const auto status=bound_buffered_input(inputs,i,node,plan,limits);if(status!=Status::ok)return status;}
    const auto* current=input_at(inputs,0);if(!current)return Status::unavailable;
    if(plan.kind!=BufferedOperationKind::Flux)return Status::ok;
    const auto* delayed=input_at(inputs,1);if(!delayed)return Status::unavailable;
    return corresponding_bands(*current,*delayed)?Status::ok:Status::incompatible_contract;
}
}
Status BufferedOperation::validate_call(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) const noexcept {
    if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs) || !proof_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
    const auto status=read_buffers(inputs,definition_view(descriptor_->node),plan_,limits_);
    return status==Status::ok?validate_outputs(outputs,limits_):status;
}
}
