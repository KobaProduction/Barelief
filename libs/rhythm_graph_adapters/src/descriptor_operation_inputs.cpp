// SPDX-License-Identifier: AGPL-3.0-or-later
#include "descriptor_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status validate_inputs(std::span<const NumericOperationInput> inputs,const domain::ResolvedProcessingNode& node,
    const DescriptorPlan& plan,const FrameLimits& limits) noexcept {
    for(std::size_t i=0;i<inputs.size();++i){
        const auto& input=inputs[i];NumericContract expected;
        if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
        for(std::size_t j=0;j<i;++j)if(inputs[j].port==input.port)return Status::invalid_argument;
        const auto status=input.port==descriptor_control_port?validate_operation_control(input,expected,limits):
            validate_frame_vector(input,node,plan.bins,limits,input.port==0);
        if(status!=Status::ok)return status;
    }
    const auto* magnitude=input_at(inputs,0);if(!magnitude)return Status::unavailable;
    if(!plan.phase)return Status::ok;
    const auto* phase=input_at(inputs,1);if(!phase)return Status::unavailable;
    return equal_frame_identity(*magnitude,*phase)?Status::ok:Status::incompatible_contract;
}
}
Status DescriptorOperation::validate_call(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) const noexcept {
    if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs) || !proof_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
    const auto status=validate_inputs(inputs,definition_view(descriptor_->node),plan_,limits_);
    return status==Status::ok?validate_outputs(outputs,limits_):status;
}
}
