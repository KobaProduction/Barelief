// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include <dsp_kernels/kernel_support.h>
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
bool valid_contracts(NumericOperationPorts ports,const FrameLimits& limits) noexcept {
    for(const auto port:ports)if(validate_contract(port.contract.view(),limits)!=Status::ok)return false;
    return true;
}
bool assessment_resources(const NumericOperationNodeView& node,const SequenceSourcePlan& plan,
    const NumericOperationProfile& profile,domain::ProcessingResources& resources) noexcept {
    if(!numeric_operation_descriptor_required_bytes(node,profile.admission,resources.plan_bytes) ||
       !numeric_operation_add_bytes(resources.plan_bytes,sizeof(SequenceSourceOperation)))return false;
    resources.scratch_bytes=plan.kernel_bytes;
    if(!numeric_operation_add_bytes(resources.scratch_bytes,plan.buffer_bytes))return false;
    resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;
    return numeric_operation_add_bytes(resources.stack_bytes,6*sizeof(SequenceSourceOperation));
}
bool outputs_match(const dataflow_runtime::NumericOperationNodeView& node,const SequenceSourcePlan& plan) noexcept {
    if(node.outputs.size()!=sequence_source_output_count(plan.kind))return false;
    for(std::size_t i=0;i<node.outputs.size();++i){
        NumericBufferContract value;if(node.outputs[i].id!=i || !numeric_port(node,false,domain::PortId(i),value))return false;
        const auto shape=sequence_source_shape(plan,domain::PortId(i));
        if(value.dtype!=shape.dtype || value.shape.rank!=shape.rank || value.plane_count!=1 ||
           value.shape.extents[0].maximum<shape.rows || value.shape.extents[0].minimum>(shape.variable?0:shape.rows) ||
           value.shape.extents[1].maximum<shape.columns || value.shape.extents[1].minimum>shape.columns)return false;
    }
    return true;
}
}
domain::ProcessingSupport assess_sequence_source(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationProfile& profile) noexcept {
    domain::ProcessingSupport support;SequenceSourcePlan plan;
    if(!valid_contracts(node.inputs,profile.admission.frame) || !valid_contracts(node.outputs,profile.admission.frame))return {};
    if(!sequence_source_plan(node,plan) || !outputs_match(node,plan) ||
       !assessment_resources(node,plan,profile,support.resources))return {};
    support.supported=true;return support;
}
NumericOperationPolicy sequence_source_policy(const dataflow_runtime::NumericOperationNodeView& node) {
    NumericOperationPolicy policy;policy.arithmetic_failure=NumericOperationFailure::PreserveState;
    if(node.kind=="ActivationPeakSequence" || node.kind=="SpectralDifferenceSequence")policy.mode=NumericOperationMode::ContinuingStream;
    policy.match=NumericInputMatch::SessionSequence;policy.session=NumericInputSession::SharedProvenance;
    policy.inputs.reserve(node.inputs.size());policy.output_values.reserve(node.outputs.size());
    for(const auto& input:node.inputs)policy.inputs.push_back({input.id,input.id?NumericInputUse::HeldValue:NumericInputUse::Frame,true,ValueDomain::FiniteOnly});
    for(std::size_t i=0;i<node.outputs.size();++i)policy.output_values.push_back(ValueDomain::FiniteOnly);
    return policy;
}
}
