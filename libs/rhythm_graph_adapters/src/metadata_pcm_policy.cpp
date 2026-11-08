// SPDX-License-Identifier: AGPL-3.0-or-later
#include "metadata_policy.h"
#include "pcm_stage_operations.h"
#include "btrack_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
void window_metadata(NumericOperationPolicy& policy) {
    NumericOperationMeaningRule meaning;meaning.direction=domain::PortDirection::Output;meaning.same_unit_as_input=0;
    policy.meanings.push_back(std::move(meaning));
    for(std::size_t axis=0;axis<2;++axis){
        NumericOperationAxisRule rule;rule.axis=axis;rule.kind=NumericOperationAxisKind::Declaration;
        rule.allow_explicit=false;rule.ignore_other_units=true;rule.default_absent=false;policy.axes.push_back(rule);
        policy.axis_matches.push_back({domain::PortDirection::Output,0,0,axis,axis,false,true});
        policy.coordinate_routes.push_back({0,0,axis,axis});
    }
}
}
void pcm_metadata_policy(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    PcmStagePlan plan;if(!plan_pcm_stage(node,plan))return;
    metadata_index_outputs(node,policy,true);
    for(const auto& input:node.inputs)if(input.id)metadata_clock(policy,domain::PortDirection::Input,input.id,NumericOperationClockKind::MatchInput);
}
namespace {
void btrack_clocks(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    metadata_clock(policy,domain::PortDirection::Input,0,NumericOperationClockKind::SampleRate,44100,false);
    for(const auto& port:node.inputs)if(port.id)metadata_clock(policy,domain::PortDirection::Input,port.id,NumericOperationClockKind::MatchInput);
    for(const auto& port:node.outputs)metadata_clock(policy,domain::PortDirection::Output,port.id,NumericOperationClockKind::MatchInput);
}
}
void btrack_metadata_policy(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    BTrackPlan plan;if(!btrack_plan(node,plan))return;
    btrack_clocks(node,policy);
    if(plan.kind==BTrackKind::Onset)metadata_match_input(policy,1);
    if(plan.kind==BTrackKind::Threshold)window_metadata(policy);
}
}
