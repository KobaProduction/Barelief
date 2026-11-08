// SPDX-License-Identifier: AGPL-3.0-or-later
#include "event_operations.h"
#include <dsp_kernels/kernel_support.h>
#include "operation_io.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
bool event_output_contracts(const dataflow_runtime::NumericOperationNodeView& node,const EventPlan& plan) noexcept {
    if(node.outputs.size()!=event_output_count(plan))return false;
    NumericBufferContract source;if(!numeric_port(node,true,0,source))return false;
    for(std::size_t i=0;i<node.outputs.size();++i){
        NumericBufferContract output;
        if(!numeric_port(node,false,domain::PortId(i),output) || output.dtype!=event_output_dtype(plan,domain::PortId(i)) ||
           output.shape.rank!=Rank::Scalar || output.plane_count!=1 ||
           output.representation.normalization!=Normalization::None)return false;
    }
    return true;
}
domain::ProcessingSupport assess_event_operation(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationProfile& profile) noexcept {
    domain::ProcessingSupport support;EventPlan plan;
    if(!operation_ports_valid(node,profile.admission.frame) || !plan_event_operation(node,plan) || !event_output_contracts(node,plan) ||
       !numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(EventOperation)))return {};
    return complete_operation_support(support,plan.kernel_bytes);
}
NumericOperationPolicy event_operation_policy(const dataflow_runtime::NumericOperationNodeView& node) {
    return frame_operation_policy(node,event_control_port,NumericOperationFailure::ResetState);
}
}
