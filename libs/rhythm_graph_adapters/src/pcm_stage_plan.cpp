// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
namespace {
bool tracker_plan(PcmStagePlan& plan) noexcept {
    plan.input_rows=plan.tracker.window;plan.history=plan.tracker.window;
    return PreparedContextBeatTracker::storage_bytes(plan.tracker,plan.kernel_bytes) &&
        dataflow_runtime::numeric_operation_mul_bytes(plan.history/4+1,sizeof(float),plan.buffer_bytes);
}
bool sequence_plan(PcmStagePlan& plan) noexcept {
    if(!PreparedContextBeatSequence::resolve_configuration(plan.sequence,plan.sequence) ||
       !PreparedContextBeatSequence::storage_bytes(plan.sequence,plan.kernel_bytes))return false;
    plan.history=plan.sequence.tracker.window;plan.input_rows=plan.history;return true;
}

}
bool plan_pcm_stage(const dataflow_runtime::NumericOperationNodeView& node,PcmStagePlan& plan) noexcept {
    plan={};if(!pcm_stage_settings(node,plan))return false;
    if(plan.kind==PcmStageKind::Tracker){if(!tracker_plan(plan))return false;}
    else if(!sequence_plan(plan))return false;
    return pcm_stage_inputs(node,plan);
}
}
