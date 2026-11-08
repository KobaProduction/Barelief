// SPDX-License-Identifier: AGPL-3.0-or-later
#include "contrast_operations.h"
#include <dsp_kernels/kernel_support.h>
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
domain::ProcessingSupport assess_contrast_operation(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationProfile& profile) noexcept {
    domain::ProcessingSupport support;ContrastPlan plan;
    if(!operation_ports_valid(node,profile.admission.frame) || !plan_contrast_operation(node,plan) || !contrast_output_contracts(node,plan) ||
       !numeric_operation_descriptor_required_bytes(node,profile.admission,support.resources.plan_bytes) ||
       !numeric_operation_add_bytes(support.resources.plan_bytes,sizeof(ContrastOperation)))return {};
    return complete_operation_support(support,plan.kernel_bytes,plan.buffer_bytes);
}
NumericOperationPolicy contrast_operation_policy(const dataflow_runtime::NumericOperationNodeView& node) {
    return frame_operation_policy(node,contrast_control_port,NumericOperationFailure::PreserveState);
}
}
