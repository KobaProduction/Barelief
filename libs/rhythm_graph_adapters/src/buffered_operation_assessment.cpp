// SPDX-License-Identifier: AGPL-3.0-or-later
#include "buffered_operations.h"
#include <dsp_kernels/kernel_support.h>
#include "operation_io.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
bool buffered_output(const dataflow_runtime::NumericOperationNodeView& node,const BufferedPlan& plan,domain::PortId id,std::uint64_t minimum) noexcept {
    NumericBufferContract output;if(!numeric_port(node,false,id,output))return false;
    const bool flux=plan.kind==BufferedOperationKind::Flux;
    const bool scalar=flux && !id;
    return output_shape(node,id,scalar?Rank::Scalar:Rank::Vector,scalar?1:plan.maximum) &&
        (flux || output.shape.extents[0].minimum<=minimum);
}
}
bool buffered_output_contracts(const dataflow_runtime::NumericOperationNodeView& node,const BufferedPlan& plan) noexcept {
    if(node.outputs.size()!=(plan.kind==BufferedOperationKind::Flux?3u:1u))return false;
    NumericBufferContract input;if(!numeric_port(node,true,0,input))return false;
    for(std::size_t i=0;i<node.outputs.size();++i)if(!buffered_output(node,plan,domain::PortId(i),input.shape.extents[0].minimum))return false;
    return true;
}
domain::ProcessingSupport assess_buffered_operation(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationProfile& profile) noexcept {
    domain::ProcessingSupport support;BufferedPlan plan;
    if(!operation_ports_valid(node,profile.admission.frame) || !plan_buffered_operation(node,plan) || !buffered_output_contracts(node,plan) ||
       !numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(BufferedOperation)))return {};
    return complete_operation_support(support,plan.kernel_bytes,plan.buffer_bytes);
}
NumericOperationPolicy buffered_operation_policy(const dataflow_runtime::NumericOperationNodeView& node) {
    auto policy=frame_operation_policy(node,buffered_control_port,NumericOperationFailure::PreserveState);
    for(auto& input:policy.inputs)if(input.port==1)input.use=NumericInputUse::HeldValue;
    return policy;
}
}
