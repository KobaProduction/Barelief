// SPDX-License-Identifier: AGPL-3.0-or-later
#include "descriptor_operations.h"
#include <dsp_kernels/kernel_support.h>
#include "operation_io.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
bool descriptor_output_contracts(const dataflow_runtime::NumericOperationNodeView& node,const DescriptorPlan& plan) noexcept {
    if(node.outputs.size()!=descriptor_output_count(plan))return false;
    NumericBufferContract magnitude;if(!numeric_port(node,true,0,magnitude))return false;
    for(std::size_t i=0;i<node.outputs.size();++i){
        NumericBufferContract output;if(!numeric_port(node,false,domain::PortId(i),output))return false;
        const auto shape=descriptor_output_shape(plan,domain::PortId(i));
        if(!output_shape(node,domain::PortId(i),shape.rank,std::size_t(shape.extents[0].maximum),std::size_t(shape.extents[1].maximum)))return false;
    }
    return true;
}
domain::ProcessingSupport assess_descriptor_operation(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationProfile& profile) noexcept {
    domain::ProcessingSupport support;DescriptorPlan plan;
    if(!operation_ports_valid(node,profile.admission.frame) || !plan_descriptor_operation(node,plan) || !descriptor_output_contracts(node,plan))return {};
    if(!numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(DescriptorOperation)))return {};
    return complete_operation_support(support,plan.kernel_bytes);
}
NumericOperationPolicy descriptor_operation_policy(const dataflow_runtime::NumericOperationNodeView& node) {
    return frame_operation_policy(node,descriptor_control_port,NumericOperationFailure::ResetState);
}
}
