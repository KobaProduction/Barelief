// SPDX-License-Identifier: AGPL-3.0-or-later
#include "metadata_policy.h"
#include "batch_operation_plan.h"
#include "tracking_operations.h"
#include "loudness_operations.h"
#include <algorithm>
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
void batch_metadata_policy(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    BatchPlan plan;if(!batch_operation_plan(node,plan))return;
    metadata_index_outputs(node,policy,false);
}
void tracking_metadata_policy(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    TrackingPlan plan;if(!tracking_operation_plan(node,plan))return;
    metadata_index_outputs(node,policy,plan.frame);
    if(plan.kind==TrackingKind::PhaseTicks){
        metadata_match_input(policy,1,1);
        policy.axis_matches.push_back({domain::PortDirection::Input,1,0,1,1,false,false});
    }

}
namespace {
void loudness_axis(const LoudnessPlan& plan,NumericOperationPolicy& policy) {
    if(plan.kind!=LoudnessKind::LoopFit && plan.kind!=LoudnessKind::Ratios && plan.kind!=LoudnessKind::Frequency)return;
    const bool frequency=plan.kind==LoudnessKind::Ratios || plan.kind==LoudnessKind::Frequency;
    const double step=frequency?double(plan.rate)/(2.*double(std::max(std::size_t{1},plan.rows-1))):
        1./double(plan.rate);
    auto rule=metadata_uniform(0,0,frequency?NumericUnit::Hertz:NumericUnit::Seconds,step);
    rule.tolerance=1e-6;rule.ignore_other_units=true;policy.axes.push_back(rule);
}
}
void loudness_metadata_policy(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    LoudnessPlan plan;if(!loudness_operation_plan(node,plan))return;
    metadata_index_outputs(node,policy,false);loudness_axis(plan,policy);
    if(plan.kind==LoudnessKind::LoopFit)metadata_clock(policy,domain::PortDirection::Input,0,NumericOperationClockKind::SampleRate,plan.rate);
}
}
