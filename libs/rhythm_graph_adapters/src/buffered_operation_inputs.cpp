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
    const auto& a=current.stamp;const auto& b=delayed.stamp;
    if(a.producer_epoch!=b.producer_epoch || a.generation!=b.generation || a.epoch!=b.epoch || a.graph_revision!=b.graph_revision)return false;
    for(std::size_t axis=0;axis<2;++axis)
        if(current.value.coordinates.axes[axis].size()!=delayed.value.coordinates.axes[axis].size() ||
           !std::equal(current.value.coordinates.axes[axis].begin(),current.value.coordinates.axes[axis].end(),delayed.value.coordinates.axes[axis].begin()))return false;
    return true;
}
Status read_buffers(std::span<const NumericOperationInput> inputs,const domain::ResolvedProcessingNode& node,
    const BufferedPlan& plan,const FrameLimits& limits) noexcept {
    for(std::size_t i=0;i<inputs.size();++i){
        const auto& input=inputs[i];NumericContract expected;if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
        for(std::size_t j=0;j<i;++j)if(inputs[j].port==input.port)return Status::invalid_argument;
        const auto status=input.port==buffered_control_port?validate_operation_control(input,expected,limits):validate_vector(input,plan.maximum,expected,limits);
        if(status!=Status::ok)return status;
    }
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
