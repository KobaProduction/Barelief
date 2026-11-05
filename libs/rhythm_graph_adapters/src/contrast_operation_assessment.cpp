// SPDX-License-Identifier: AGPL-3.0-or-later
#include "contrast_operations.h"

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
domain::ProcessingSupport assess_contrast_operation(const domain::ResolvedProcessingNode& node,const NumericRuntimeProfile& profile) noexcept {
    domain::ProcessingSupport support;ContrastPlan plan;
    for(const auto& input:node.inputs)if(validate_contract(input.contract.view(),profile.admission.frame)!=Status::ok)return {};
    for(const auto& output:node.outputs)if(validate_contract(output.contract.view(),profile.admission.frame)!=Status::ok)return {};
    if(!plan_contrast_operation(node,plan) || !contrast_output_contracts(node,plan) ||
       !numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(ContrastOperation)))return {};
    support.resources.scratch_bytes=plan.kernel_bytes;
    if(!numeric_operation_add_bytes(support.resources.scratch_bytes,plan.buffer_bytes))return {};
    support.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;support.supported=true;return support;
}
NumericOperationPolicy contrast_operation_policy(const domain::ResolvedProcessingNode& node) {
    NumericOperationPolicy policy;policy.mode=NumericOperationMode::ContinuingStream;
    policy.match=NumericInputMatch::SessionSequence;policy.session=NumericInputSession::SharedProvenance;
    policy.arithmetic_failure=NumericOperationFailure::PreserveState;policy.inputs.reserve(node.inputs.size());
    for(const auto& input:node.inputs){
        const bool control=input.id==contrast_control_port;
        policy.inputs.push_back({input.id,control?NumericInputUse::Control:NumericInputUse::Frame,!control,ValueDomain::FiniteOnly});
        if(control)policy.control=input.id;
    }
    policy.output_values.reserve(node.outputs.size());
    for(std::size_t i=0;i<node.outputs.size();++i)policy.output_values.push_back(ValueDomain::FiniteOnly);
    return policy;
}
}
