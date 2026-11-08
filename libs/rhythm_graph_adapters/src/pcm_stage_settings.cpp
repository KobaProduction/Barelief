// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "parameter_values.h"
#include <utility>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
namespace {
bool integer32(const dataflow_runtime::NumericOperationNodeView& node,std::string_view key,std::uint32_t fallback,std::uint32_t& output) noexcept {
    std::size_t value=0;if(!integer_parameter(node,key,fallback,value) || !std::in_range<std::uint32_t>(value))return false;
    output=std::uint32_t(value);return true;
}
bool signed32(const dataflow_runtime::NumericOperationNodeView& node,std::string_view key,std::int32_t& output) noexcept {
    const auto* setting=parameter(node,key);if(!setting)return true;
    const auto* literal=std::get_if<signal_contracts::NumericLiteral>(setting);if(!literal)return false;
    return std::visit([&](auto value){
        if constexpr(!std::is_integral_v<decltype(value)>)return false;
        else {if(!std::in_range<std::int32_t>(value))return false;output=std::int32_t(value);return true;}
    },*literal);
}
bool tracker_settings(const dataflow_runtime::NumericOperationNodeView& node,ContextBeatTrackerConfig& tracker,std::size_t window) noexcept {
    return integer_parameter(node,"tracker_window",window,tracker.window) &&
        scalar_parameter(node,"frame_rate",44100.f/512.f,tracker.frame_rate) &&
        scalar_parameter(node,"preferred_bpm",120,tracker.preferred_bpm) &&
        scalar_parameter(node,"context_variance",3.901f,tracker.context_variance) &&
        scalar_parameter(node,"min_period_frames",25,tracker.min_period_frames);
}
bool sequence_settings(const dataflow_runtime::NumericOperationNodeView& node,ContextBeatSequenceConfig& sequence) noexcept {
    std::size_t signature=4;
    if(!tracker_settings(node,sequence.tracker,0) || !integer32(node,"hop_samples",512,sequence.hop_samples) ||
       !signed32(node,"delay_samples",sequence.delay_samples) || !integer_parameter(node,"tatums_per_beat",4,signature) ||
       !std::in_range<unsigned>(signature))return false;
    sequence.tatums_per_beat=unsigned(signature);return true;
}
}
bool pcm_stage_operation_kind(std::string_view kind) noexcept {
    return kind=="ContextBeatTracker" || kind=="ContextBeatSequence";
}
bool pcm_stage_settings(const dataflow_runtime::NumericOperationNodeView& node,PcmStagePlan& plan) noexcept {
    if(node.kind=="ContextBeatTracker"){
        plan.kind=PcmStageKind::Tracker;return tracker_settings(node,plan.tracker,512);
    }
    if(node.kind=="ContextBeatSequence"){
        plan.kind=PcmStageKind::Sequence;return sequence_settings(node,plan.sequence);
    }
    return false;
}
}
