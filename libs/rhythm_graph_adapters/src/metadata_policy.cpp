// SPDX-License-Identifier: AGPL-3.0-or-later
#include "metadata_policy.h"
#include "adapter_operation.h"
#include "spectral_operation_plan.h"
#include "descriptor_operations.h"
#include "event_operations.h"
#include "buffered_operations.h"
#include "contrast_operations.h"
#include "pcm_stage_operations.h"
#include "batch_operation_plan.h"
#include "tracking_operations.h"
#include "loudness_operations.h"
#include "sequence_source_operations.h"
#include "btrack_operations.h"
#include "essentia_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
void metadata_clock(NumericOperationPolicy& policy,domain::PortDirection direction,domain::PortId port,
    NumericOperationClockKind kind,double rate,bool unknown) {
    NumericOperationClockRule rule;rule.direction=direction;rule.port=port;
    rule.kind=kind;rule.sample_rate=rate;rule.allow_unknown=kind==NumericOperationClockKind::MatchInput?false:unknown;policy.clocks.push_back(rule);
}
void metadata_index_axes(NumericOperationPolicy& policy,domain::PortId port) {
    for(std::size_t axis=0;axis<2;++axis){
        NumericOperationAxisRule rule;rule.direction=domain::PortDirection::Output;rule.port=port;rule.axis=axis;
        rule.allow_explicit=false;policy.axes.push_back(rule);
    }
}
void metadata_index_outputs(const NumericOperationNodeView& node,NumericOperationPolicy& policy,bool clocked) {
    for(const auto& port:node.outputs){
        metadata_clock(policy,domain::PortDirection::Output,port.id,clocked?NumericOperationClockKind::MatchInput:NumericOperationClockKind::Unclocked);
        metadata_index_axes(policy,port.id);
    }
}
void metadata_match_input(NumericOperationPolicy& policy,domain::PortId port,std::size_t axes,bool compare_tags) {
    metadata_clock(policy,domain::PortDirection::Input,port,NumericOperationClockKind::MatchInput);
    for(std::size_t axis=0;axis<axes;++axis)policy.axis_matches.push_back({domain::PortDirection::Input,port,0,axis,axis,true,compare_tags});
}
NumericOperationAxisRule metadata_uniform(domain::PortId port,std::size_t axis,NumericUnit unit,double step) {
    NumericOperationAxisRule rule;rule.port=port;rule.axis=axis;rule.kind=NumericOperationAxisKind::Uniform;
    rule.unit=unit;rule.step=step;rule.default_absent=false;return rule;
}
namespace {
bool reference_metadata(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    if(essentia_operation_kind(node.kind)){essentia_metadata_policy(node,policy);return true;}
    if(btrack_operation_kind(node.kind)){btrack_metadata_policy(node,policy);return true;}
    return false;
}
void remaining_metadata(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    if(interval_operation_kind(node.kind) || sequence_source_kind(node.kind)){metadata_index_outputs(node,policy,false);return;}
    if(pcm_stage_operation_kind(node.kind)){pcm_metadata_policy(node,policy);return;}
    feature_metadata_policy(node,policy);
}
}
void add_metadata_policy(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    if(reference_metadata(node,policy))return;
    if(batch_operation_kind(node.kind)){batch_metadata_policy(node,policy);return;}
    if(tracking_operation_kind(node.kind)){tracking_metadata_policy(node,policy);return;}
    if(loudness_operation_kind(node.kind)){loudness_metadata_policy(node,policy);return;}
    remaining_metadata(node,policy);
}
}
