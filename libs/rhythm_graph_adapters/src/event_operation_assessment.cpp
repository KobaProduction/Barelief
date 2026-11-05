// SPDX-License-Identifier: AGPL-3.0-or-later
#include "event_operations.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
bool event_output_contracts(const domain::ResolvedProcessingNode& node,const EventPlan& plan) noexcept {
    if(node.outputs.size()!=event_output_count(plan))return false;
    NumericContract source;if(!numeric_port(node,true,0,source))return false;
    for(std::size_t i=0;i<node.outputs.size();++i){
        NumericContract output;
        if(!numeric_port(node,false,domain::PortId(i),output) || output.dtype!=event_output_dtype(plan,domain::PortId(i)) ||
           output.shape.rank!=Rank::Scalar || output.channel_order.size()!=1 ||
           output.representation.normalization!=Normalization::None || !equal_frame_clock(output.clock,source.clock))return false;
        output.clock={};if(!index_output_contract(output))return false;
    }
    return true;
}
domain::ProcessingSupport assess_event_operation(const domain::ResolvedProcessingNode& node,const NumericRuntimeProfile& profile) noexcept {
    domain::ProcessingSupport support;EventPlan plan;
    for(const auto& input:node.inputs)if(validate_contract(input.contract.view(),profile.admission.frame)!=Status::ok)return {};
    for(const auto& output:node.outputs)if(validate_contract(output.contract.view(),profile.admission.frame)!=Status::ok)return {};
    if(!plan_event_operation(node,plan) || !event_output_contracts(node,plan) ||
       !numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(EventOperation)))return {};
    support.resources.scratch_bytes=plan.kernel_bytes;
    support.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;support.supported=true;return support;
}
NumericOperationPolicy event_operation_policy(const domain::ResolvedProcessingNode& node) {
    NumericOperationPolicy policy;policy.mode=NumericOperationMode::ContinuingStream;
    policy.match=NumericInputMatch::SessionSequence;policy.session=NumericInputSession::SharedProvenance;
    policy.arithmetic_failure=NumericOperationFailure::ResetState;policy.inputs.reserve(node.inputs.size());
    for(const auto& input:node.inputs){
        const bool control=input.id==event_control_port;
        policy.inputs.push_back({input.id,control?NumericInputUse::Control:NumericInputUse::Frame,!control,ValueDomain::FiniteOnly});
        if(control)policy.control=input.id;
    }
    policy.output_values.reserve(node.outputs.size());
    for(std::size_t i=0;i<node.outputs.size();++i)policy.output_values.push_back(ValueDomain::FiniteOnly);
    return policy;
}
}
