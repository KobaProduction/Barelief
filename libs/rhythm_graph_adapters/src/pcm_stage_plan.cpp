// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
namespace {
bool coefficient_setting(const domain::ResolvedProcessingNode& node,std::span<const float>& coefficients) noexcept {
    coefficients={};const auto* setting=parameter(node,"coefficients");if(!setting)return true;
    const auto* array=std::get_if<domain::NumericArray>(setting);if(!array)return false;
    coefficients=array->values;return true;
}
bool spectrum_plan(const domain::ResolvedProcessingNode& node,PcmStagePlan& plan) noexcept {
    plan.bins=std::size_t(plan.onset.spectrum.window_samples)/2+1;plan.input_rows=plan.onset.spectrum.hop_samples;
    if(plan.kind==PcmStageKind::Onset)return PreparedPcmOnsetSequence::storage_bytes(plan.onset,plan.kernel_bytes);
    if(plan.kind==PcmStageKind::Spectrum){
        std::span<const float> coefficients;
        return coefficient_setting(node,coefficients) && PreparedHopSpectrum::storage_bytes(plan.onset.spectrum,coefficients,plan.kernel_bytes) &&
            dataflow_runtime::numeric_operation_mul_bytes(plan.bins,2*sizeof(float),plan.buffer_bytes);
    }
    plan.sequence=plan.tempo.sequence;plan.sequence.hop_samples=plan.tempo.spectrum.hop_samples;
    plan.sequence.tracker.frame_rate=float(plan.sample_rate)/float(plan.tempo.spectrum.hop_samples);
    if(!PreparedContextBeatSequence::resolve_configuration(plan.sequence,plan.sequence))return false;
    plan.history=plan.sequence.tracker.window;
    return PreparedPcmContextTempo::storage_bytes(plan.tempo,plan.kernel_bytes);
}
}
bool plan_pcm_stage(const domain::ResolvedProcessingNode& node,PcmStagePlan& plan) noexcept {
    plan={};if(!pcm_stage_settings(node,plan))return false;
    if(plan.kind==PcmStageKind::Tracker){
        plan.input_rows=plan.tracker.window;plan.history=plan.tracker.window;
        if(!PreparedContextBeatTracker::storage_bytes(plan.tracker,plan.kernel_bytes) ||
           !dataflow_runtime::numeric_operation_mul_bytes(plan.history/4+1,sizeof(float),plan.buffer_bytes))return false;
    }else if(plan.kind==PcmStageKind::Sequence){
        if(!PreparedContextBeatSequence::resolve_configuration(plan.sequence,plan.sequence) ||
           !PreparedContextBeatSequence::storage_bytes(plan.sequence,plan.kernel_bytes))return false;
        plan.history=plan.sequence.tracker.window;plan.input_rows=1;
    }else if(!spectrum_plan(node,plan))return false;
    return pcm_stage_inputs(node,plan);
}
}
