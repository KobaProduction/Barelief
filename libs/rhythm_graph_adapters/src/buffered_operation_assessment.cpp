// SPDX-License-Identifier: AGPL-3.0-or-later
#include "buffered_operations.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
bool buffered_output_contracts(const domain::ResolvedProcessingNode& node,const BufferedPlan& plan) noexcept {
    const bool flux=plan.kind==BufferedOperationKind::Flux;
    if(node.outputs.size()!=(flux?3u:1u))return false;
    NumericContract input;if(!numeric_port(node,true,0,input))return false;
    for(std::size_t i=0;i<node.outputs.size();++i){
        NumericContract output;if(!numeric_port(node,false,domain::PortId(i),output) || !equal_frame_clock(output.clock,input.clock))return false;
        output.clock={};if(!index_output_contract(output))return false;
        const bool scalar=flux && !i;const auto minimum=std::size_t(input.shape.extents[0].minimum);
        if(!output_shape(node,domain::PortId(i),scalar?Rank::Scalar:Rank::Vector,scalar?1:plan.maximum))return false;
        if(!flux && output.shape.extents[0].minimum>minimum)return false;
    }
    return true;
}
domain::ProcessingSupport assess_buffered_operation(const domain::ResolvedProcessingNode& node,const NumericRuntimeProfile& profile) noexcept {
    domain::ProcessingSupport support;BufferedPlan plan;
    for(const auto& input:node.inputs)if(validate_contract(input.contract.view(),profile.admission.frame)!=Status::ok)return {};
    for(const auto& output:node.outputs)if(validate_contract(output.contract.view(),profile.admission.frame)!=Status::ok)return {};
    if(!plan_buffered_operation(node,plan) || !buffered_output_contracts(node,plan) ||
       !numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(BufferedOperation)))return {};
    support.resources.scratch_bytes=plan.kernel_bytes;
    if(!numeric_operation_add_bytes(support.resources.scratch_bytes,plan.buffer_bytes))return {};
    support.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;support.supported=true;return support;
}
NumericOperationPolicy buffered_operation_policy(const domain::ResolvedProcessingNode& node) {
    NumericOperationPolicy policy;policy.mode=NumericOperationMode::ContinuingStream;
    policy.match=NumericInputMatch::SessionSequence;policy.session=NumericInputSession::SharedProvenance;
    policy.arithmetic_failure=NumericOperationFailure::PreserveState;policy.inputs.reserve(node.inputs.size());
    for(const auto& input:node.inputs){
        const auto use=input.id==buffered_control_port?NumericInputUse::Control:input.id==1?NumericInputUse::HeldValue:NumericInputUse::Frame;
        policy.inputs.push_back({input.id,use,use!=NumericInputUse::Control,ValueDomain::FiniteOnly});
        if(use==NumericInputUse::Control)policy.control=input.id;
    }
    policy.output_values.reserve(node.outputs.size());
    for(std::size_t i=0;i<node.outputs.size();++i)policy.output_values.push_back(ValueDomain::FiniteOnly);
    return policy;
}
}
