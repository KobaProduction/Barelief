// SPDX-License-Identifier: AGPL-3.0-or-later
#include "spectral_operations.h"
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
bool frame_output(const domain::ResolvedProcessingNode& node,domain::PortId port,Rank rank,std::size_t rows,
    const ClockContract& source_clock) noexcept {
    NumericContract value;if(!spectral_port(node,false,port,value))return false;
    if(!spectral_same_clock(value.clock,source_clock))return false;
    value.clock={};
    return index_output_contract(value) && output_shape(node,port,rank,rows);
}
bool valid_contracts(const domain::ResolvedProcessingNode& node,const FrameLimits& limits) noexcept {
    for(const auto& input:node.inputs)if(validate_contract(input.contract.view(),limits)!=Status::ok)return false;
    for(const auto& output:node.outputs)if(validate_contract(output.contract.view(),limits)!=Status::ok)return false;
    return true;
}
}
bool spectral_output_contracts(const domain::ResolvedProcessingNode& node,const SpectralOperationPlan& plan) noexcept {
    NumericContract magnitude;if(!spectral_port(node,true,0,magnitude))return false;
    const auto& clock=magnitude.clock;
    if(plan.kind==SpectralOperationKind::Rms)return node.outputs.size()==1 && frame_output(node,0,Rank::Scalar,1,clock);
    if(plan.kind==SpectralOperationKind::Polar)return node.outputs.size()==1 && frame_output(node,0,Rank::Vector,plan.bins,clock);
    if(plan.kind==SpectralOperationKind::Histogram)
        return node.outputs.size()==3 && frame_output(node,0,Rank::Scalar,1,clock) &&
            frame_output(node,1,Rank::Vector,plan.histogram.histogram.bins,clock) && frame_output(node,2,Rank::Vector,plan.bins,clock);
    return node.outputs.size()==2 && frame_output(node,0,Rank::Vector,plan.bins,clock) && frame_output(node,1,Rank::Vector,plan.bins,clock);
}
domain::ProcessingSupport assess_spectral_operation(const domain::ResolvedProcessingNode& node,const NumericRuntimeProfile& profile) noexcept {
    domain::ProcessingSupport support;SpectralOperationPlan plan;
    if(!valid_contracts(node,profile.admission.frame) || !plan_spectral_operation(node,plan) || !spectral_output_contracts(node,plan))return support;
    if(!numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(SpectralOperation)))return {};
    support.resources.scratch_bytes=plan.kernel_bytes;
    if(!numeric_operation_add_bytes(support.resources.scratch_bytes,plan.output_buffer_bytes))return {};
    support.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;support.supported=true;return support;
}
NumericOperationPolicy spectral_operation_policy(const domain::ResolvedProcessingNode& node) {
    NumericOperationPolicy policy;policy.mode=NumericOperationMode::ContinuingStream;policy.match=NumericInputMatch::SessionSequence;
    policy.session=NumericInputSession::SharedProvenance;
    policy.arithmetic_failure=NumericOperationFailure::ResetState;policy.inputs.reserve(node.inputs.size());
    for(const auto& port:node.inputs){
        const bool frame=port.id<2;
        const auto use=frame?NumericInputUse::Frame:port.id==spectral_control_port?NumericInputUse::Control:NumericInputUse::HeldValue;
        policy.inputs.push_back({port.id,use,frame,ValueDomain::FiniteOnly});
        if(port.id==spectral_control_port)policy.control=port.id;
    }
    policy.output_values.reserve(node.outputs.size());
    for(std::size_t i=0;i<node.outputs.size();++i)policy.output_values.push_back(ValueDomain::FiniteOnly);
    return policy;
}
}
