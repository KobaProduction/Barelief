// SPDX-License-Identifier: AGPL-3.0-or-later
#include "spectral_operations.h"
#include <dsp_kernels/kernel_support.h>
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
bool frame_output(const dataflow_runtime::NumericOperationNodeView& node,domain::PortId port,Rank rank,std::size_t rows) noexcept {
    NumericBufferContract value;if(!spectral_port(node,false,port,value))return false;
    return output_shape(node,port,rank,rows);
}
bool valid_contracts(const dataflow_runtime::NumericOperationNodeView& node,const FrameLimits& limits) noexcept {
    for(const auto& input:node.inputs)if(validate_contract(input.contract.view(),limits)!=Status::ok)return false;
    for(const auto& output:node.outputs)if(validate_contract(output.contract.view(),limits)!=Status::ok)return false;
    return true;
}
}
bool spectral_output_contracts(const dataflow_runtime::NumericOperationNodeView& node,const SpectralOperationPlan& plan) noexcept {
    NumericBufferContract magnitude;if(!spectral_port(node,true,0,magnitude))return false;
    if(plan.kind==SpectralOperationKind::Rms)return node.outputs.size()==1 && frame_output(node,0,Rank::Scalar,1);
    if(plan.kind==SpectralOperationKind::Polar)return node.outputs.size()==1 && frame_output(node,0,Rank::Vector,plan.bins);
    if(plan.kind==SpectralOperationKind::Histogram)
        return node.outputs.size()==3 && frame_output(node,0,Rank::Scalar,1) &&
            frame_output(node,1,Rank::Vector,plan.histogram.histogram.bins) && frame_output(node,2,Rank::Vector,plan.bins);
    return node.outputs.size()==2 && frame_output(node,0,Rank::Vector,plan.bins) && frame_output(node,1,Rank::Vector,plan.bins);
}
domain::ProcessingSupport assess_spectral_operation(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationProfile& profile) noexcept {
    domain::ProcessingSupport support;SpectralOperationPlan plan;
    if(!valid_contracts(node,profile.admission.frame) || !plan_spectral_operation(node,plan) || !spectral_output_contracts(node,plan))return support;
    if(!numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(SpectralOperation)))return {};
    support.resources.scratch_bytes=plan.kernel_bytes;
    if(!numeric_operation_add_bytes(support.resources.scratch_bytes,plan.output_buffer_bytes))return {};
    support.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;support.supported=true;return support;
}
NumericOperationPolicy spectral_operation_policy(const dataflow_runtime::NumericOperationNodeView& node) {
    auto policy=frame_operation_policy(node,spectral_control_port,NumericOperationFailure::ResetState);
    for(auto& input:policy.inputs)if(input.port>=2 && input.port!=spectral_control_port){input.use=NumericInputUse::HeldValue;input.required=false;}
    return policy;
}
}
