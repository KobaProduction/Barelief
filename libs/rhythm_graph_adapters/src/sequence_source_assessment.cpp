// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
bool outputs_match(const domain::ResolvedProcessingNode& node,const SequenceSourcePlan& plan) noexcept {
    if(node.outputs.size()!=sequence_source_output_count(plan.kind))return false;
    for(std::size_t i=0;i<node.outputs.size();++i){
        NumericContract value;if(node.outputs[i].id!=i || !numeric_port(node,false,domain::PortId(i),value) || !index_output_contract(value))return false;
        const auto shape=sequence_source_shape(plan,domain::PortId(i));
        if(value.dtype!=shape.dtype || value.shape.rank!=shape.rank || value.channel_order.size()!=1 ||
           value.shape.extents[0].maximum<shape.rows || value.shape.extents[0].minimum>(shape.variable?0:shape.rows) ||
           value.shape.extents[1].maximum<shape.columns || value.shape.extents[1].minimum>shape.columns)return false;
    }
    return true;
}
}
domain::ProcessingSupport assess_sequence_source(const domain::ResolvedProcessingNode& node,const NumericRuntimeProfile& profile) noexcept {
    domain::ProcessingSupport support;SequenceSourcePlan plan;
    for(const auto& input:node.inputs)if(validate_contract(input.contract.view(),profile.admission.frame)!=Status::ok)return {};
    for(const auto& output:node.outputs)if(validate_contract(output.contract.view(),profile.admission.frame)!=Status::ok)return {};
    if(!sequence_source_plan(node,plan) || !outputs_match(node,plan) ||
       !numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(SequenceSourceOperation)))return {};
    support.resources.scratch_bytes=plan.kernel_bytes;
    if(!numeric_operation_add_bytes(support.resources.scratch_bytes,plan.buffer_bytes))return {};
    support.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;
    if(!numeric_operation_add_bytes(support.resources.stack_bytes,6*sizeof(SequenceSourceOperation)))return {};
    support.supported=true;return support;
}
NumericOperationPolicy sequence_source_policy(const domain::ResolvedProcessingNode& node) {
    NumericOperationPolicy policy;policy.arithmetic_failure=NumericOperationFailure::PreserveState;
    if(node.kind=="ActivationPeakSequence" || node.kind=="SpectralDifferenceSequence" || node.kind=="PcmSpectralDifference")policy.mode=NumericOperationMode::ContinuingStream;
    policy.match=NumericInputMatch::SessionSequence;policy.session=NumericInputSession::SharedProvenance;
    policy.inputs.reserve(node.inputs.size());policy.output_values.reserve(node.outputs.size());
    for(const auto& input:node.inputs)policy.inputs.push_back({input.id,input.id?NumericInputUse::HeldValue:NumericInputUse::Frame,true,ValueDomain::FiniteOnly});
    for(std::size_t i=0;i<node.outputs.size();++i)policy.output_values.push_back(ValueDomain::FiniteOnly);
    return policy;
}
}
