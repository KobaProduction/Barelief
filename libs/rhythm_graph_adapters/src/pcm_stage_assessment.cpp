// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
bool output_axis(NumericContract contract,PcmStageAxis axis) noexcept {
    contract.clock={};
    if(axis==PcmStageAxis::Index)return index_output_contract(contract);
    const AxisCoordinates expected{CoordinateMode::ExplicitFloat64LE,axis==PcmStageAxis::Frequency?NumericUnit::Hertz:NumericUnit::Seconds};
    return contract.coordinates[0]==expected && contract.coordinates[1]==AxisCoordinates{};
}
bool output_shape_matches(const NumericContract& output,const PcmStageOutputSpec& spec) noexcept {
    return output.dtype==spec.dtype && output.shape.rank==spec.rank && output.channel_order.size()==1 &&
        output.representation.normalization==Normalization::None && output.shape.extents[0].maximum>=spec.rows &&
        output.shape.extents[0].minimum<=(spec.variable?0:spec.rows) && output.shape.extents[1].maximum>=spec.columns &&
        output.shape.extents[1].minimum<=spec.columns && output_axis(output,spec.axis);
}
}
bool pcm_stage_output_contracts(const domain::ResolvedProcessingNode& node,const PcmStagePlan& plan) noexcept {
    if(node.outputs.size()!=pcm_stage_output_count(plan))return false;
    NumericContract source;if(!numeric_port(node,true,0,source))return false;
    for(std::size_t i=0;i<node.outputs.size();++i){
        NumericContract output;if(!numeric_port(node,false,domain::PortId(i),output) || !equal_frame_clock(source.clock,output.clock) ||
            !output_shape_matches(output,pcm_stage_output_spec(plan,domain::PortId(i))))return false;
    }
    return true;
}
domain::ProcessingSupport assess_pcm_stage(const domain::ResolvedProcessingNode& node,const NumericRuntimeProfile& profile) noexcept {
    domain::ProcessingSupport support;PcmStagePlan plan;
    for(const auto& input:node.inputs)if(validate_contract(input.contract.view(),profile.admission.frame)!=Status::ok)return {};
    for(const auto& output:node.outputs)if(validate_contract(output.contract.view(),profile.admission.frame)!=Status::ok)return {};
    if(!plan_pcm_stage(node,plan) || !pcm_stage_output_contracts(node,plan) ||
       !numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(PcmStageOperation)))return {};
    support.resources.scratch_bytes=plan.kernel_bytes;
    if(!numeric_operation_add_bytes(support.resources.scratch_bytes,plan.buffer_bytes))return {};
    support.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;support.supported=true;return support;
}
NumericOperationPolicy pcm_stage_policy(const domain::ResolvedProcessingNode& node) {
    PcmStagePlan plan;const bool planned=plan_pcm_stage(node,plan);
    NumericOperationPolicy policy;policy.mode=NumericOperationMode::ContinuingStream;
    policy.match=NumericInputMatch::SessionSequence;policy.session=NumericInputSession::SharedProvenance;
    policy.arithmetic_failure=NumericOperationFailure::PreserveState;policy.inputs.reserve(node.inputs.size());
    for(const auto& input:node.inputs){
        const bool frame=input.id==0 || (input.id==1 && node.kind=="ContextBeatSequence");
        const auto use=frame?NumericInputUse::Frame:input.id==pcm_control_port?NumericInputUse::Control:NumericInputUse::HeldValue;
        policy.inputs.push_back({input.id,use,frame,ValueDomain::FiniteOnly});
        if(use==NumericInputUse::Control)policy.control=input.id;
    }
    policy.output_values.reserve(node.outputs.size());
    for(std::size_t i=0;i<node.outputs.size();++i)policy.output_values.push_back(planned?
        pcm_stage_output_spec(plan,domain::PortId(i)).values:ValueDomain::FiniteOnly);
    return policy;
}
}
