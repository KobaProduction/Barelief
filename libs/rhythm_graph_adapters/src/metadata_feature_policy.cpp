// SPDX-License-Identifier: AGPL-3.0-or-later
#include "metadata_policy.h"
#include "spectral_operation_plan.h"
#include "descriptor_operations.h"
#include "event_operations.h"
#include "buffered_operations.h"
#include "contrast_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
namespace {
void spectral_pair(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    SpectralOperationPlan plan;
    if(plan_spectral_operation(node,plan) && plan.phase_input())metadata_match_input(policy,1);
}
void descriptor_pair(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    DescriptorPlan plan;
    if(plan_descriptor_operation(node,plan) && plan.phase)metadata_match_input(policy,1);
}
void buffered_pair(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    BufferedPlan plan;
    if(plan_buffered_operation(node,plan) && plan.kind==BufferedOperationKind::Flux)metadata_match_input(policy,1);
}
void event_pair(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    EventPlan plan;
    if(plan_event_operation(node,plan) && plan.kind==EventOperationKind::Decision)
        metadata_clock(policy,domain::PortDirection::Input,1,NumericOperationClockKind::MatchInput);
}
void feature_pairs(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    if(spectral_operation_kind(node.kind)){spectral_pair(node,policy);return;}
    if(descriptor_operation_kind(node.kind)){descriptor_pair(node,policy);return;}
    if(buffered_operation_kind(node.kind)){buffered_pair(node,policy);return;}
    if(event_operation_kind(node.kind))event_pair(node,policy);
}
}
void feature_metadata_policy(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    feature_pairs(node,policy);
    if(!contrast_operation_kind(node.kind)){metadata_index_outputs(node,policy,true);return;}
    for(const auto& port:node.outputs){
        metadata_index_axes(policy,port.id);
        metadata_clock(policy,domain::PortDirection::Output,port.id,port.id?NumericOperationClockKind::MatchInput:NumericOperationClockKind::Unclocked);
    }
}
}
