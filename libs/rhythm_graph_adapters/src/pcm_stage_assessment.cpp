// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include <dsp_kernels/kernel_support.h>
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {

bool output_shape_matches(const NumericBufferContract& output,const PcmStageOutputSpec& spec) noexcept {
    return output.dtype==spec.dtype && output.shape.rank==spec.rank && output.plane_count==1 &&
        output.representation.normalization==Normalization::None && output.shape.extents[0].maximum>=spec.rows &&
        output.shape.extents[0].minimum<=(spec.variable?0:spec.rows) && output.shape.extents[1].maximum>=spec.columns &&
        output.shape.extents[1].minimum<=spec.columns;
}
}
bool pcm_stage_output_contracts(const dataflow_runtime::NumericOperationNodeView& node,const PcmStagePlan& plan) noexcept {
    if(node.outputs.size()!=pcm_stage_output_count(plan))return false;
    NumericBufferContract source;if(!numeric_port(node,true,0,source))return false;
    for(std::size_t i=0;i<node.outputs.size();++i){
        NumericBufferContract output;if(!numeric_port(node,false,domain::PortId(i),output) ||
            !output_shape_matches(output,pcm_stage_output_spec(plan,domain::PortId(i))))return false;
    }
    return true;
}
domain::ProcessingSupport assess_pcm_stage(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationProfile& profile) noexcept {
    domain::ProcessingSupport support;PcmStagePlan plan;
    if(!operation_ports_valid(node,profile.admission.frame))return {};
    if(!plan_pcm_stage(node,plan) || !pcm_stage_output_contracts(node,plan) ||
       !numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(PcmStageOperation)))return {};
    return complete_operation_support(support,plan.kernel_bytes,plan.buffer_bytes);
}
namespace {
void pcm_input_rules(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    policy.inputs.reserve(node.inputs.size());
    for(const auto& input:node.inputs){
        const bool frame=input.id==0 || (input.id==1 && node.kind=="ContextBeatSequence");
        const auto use=frame?NumericInputUse::Frame:input.id==pcm_control_port?NumericInputUse::Control:NumericInputUse::HeldValue;
        policy.inputs.push_back({input.id,use,frame,ValueDomain::FiniteOnly});
        if(use==NumericInputUse::Control)policy.control=input.id;
    }
}
}
NumericOperationPolicy pcm_stage_policy(const NumericOperationNodeView& node) {
    PcmStagePlan plan;const bool planned=plan_pcm_stage(node,plan);
    NumericOperationPolicy policy;policy.mode=NumericOperationMode::ContinuingStream;
    policy.match=NumericInputMatch::SessionSequence;policy.session=NumericInputSession::SharedProvenance;
    policy.arithmetic_failure=NumericOperationFailure::PreserveState;pcm_input_rules(node,policy);
    policy.output_values.reserve(node.outputs.size());
    for(std::size_t i=0;i<node.outputs.size();++i)policy.output_values.push_back(planned?
        pcm_stage_output_spec(plan,domain::PortId(i)).values:ValueDomain::FiniteOnly);
    return policy;
}
}
