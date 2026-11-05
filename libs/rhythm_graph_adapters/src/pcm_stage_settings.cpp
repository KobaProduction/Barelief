// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "parameter_values.h"
#include <utility>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
namespace {
bool integer32(const domain::ResolvedProcessingNode& node,std::string_view key,std::uint32_t fallback,std::uint32_t& output) noexcept {
    std::size_t value=0;if(!integer_parameter(node,key,fallback,value) || !std::in_range<std::uint32_t>(value))return false;
    output=std::uint32_t(value);return true;
}
bool signed32(const domain::ResolvedProcessingNode& node,std::string_view key,std::int32_t& output) noexcept {
    const auto* setting=parameter(node,key);if(!setting)return true;
    const auto* literal=std::get_if<signal_contracts::NumericLiteral>(setting);if(!literal)return false;
    return std::visit([&](auto value){
        if constexpr(!std::is_integral_v<decltype(value)>)return false;
        else {if(!std::in_range<std::int32_t>(value))return false;output=std::int32_t(value);return true;}
    },*literal);
}
bool spectrum_settings(const domain::ResolvedProcessingNode& node,PcmStagePlan& plan) noexcept {
    std::size_t window=3,method=plan.kind==PcmStageKind::Tempo?7:1;
    if(!integer32(node,"sample_rate",44100,plan.sample_rate) || !integer32(node,"window_samples",1024,plan.onset.spectrum.window_samples) ||
       !integer32(node,"hop_samples",512,plan.onset.spectrum.hop_samples) || !integer_parameter(node,"window_shape",3,window) || window>8 ||
       !integer_parameter(node,"method",method,method) || method>9)return false;
    plan.onset.spectrum.window=AnalysisWindowShape(window);plan.onset.method=OnsetRecipe(method);plan.onset.sample_rate=plan.sample_rate;
    plan.tempo.spectrum=plan.onset.spectrum;plan.tempo.sample_rate=plan.sample_rate;plan.tempo.method=plan.onset.method;
    return plan.sample_rate && plan.sample_rate<=INT32_MAX;
}
bool tracker_settings(const domain::ResolvedProcessingNode& node,ContextBeatTrackerConfig& tracker,std::size_t window) noexcept {
    return integer_parameter(node,"tracker_window",window,tracker.window) &&
        scalar_parameter(node,"frame_rate",44100.f/512.f,tracker.frame_rate) &&
        scalar_parameter(node,"preferred_bpm",120,tracker.preferred_bpm) &&
        scalar_parameter(node,"context_variance",3.901f,tracker.context_variance) &&
        scalar_parameter(node,"min_period_frames",25,tracker.min_period_frames);
}
bool sequence_settings(const domain::ResolvedProcessingNode& node,ContextBeatSequenceConfig& sequence) noexcept {
    std::size_t signature=4;
    if(!tracker_settings(node,sequence.tracker,0) || !integer32(node,"hop_samples",512,sequence.hop_samples) ||
       !signed32(node,"delay_samples",sequence.delay_samples) || !integer_parameter(node,"tatums_per_beat",4,signature) ||
       !std::in_range<unsigned>(signature))return false;
    sequence.tatums_per_beat=unsigned(signature);return true;
}
}
bool pcm_stage_operation_kind(std::string_view kind) noexcept {
    return kind=="HopSpectrum" || kind=="PcmOnsetSequence" || kind=="PcmContextTempo" ||
        kind=="ContextBeatTracker" || kind=="ContextBeatSequence";
}
bool pcm_stage_settings(const domain::ResolvedProcessingNode& node,PcmStagePlan& plan) noexcept {
    if(node.kind=="HopSpectrum")plan.kind=PcmStageKind::Spectrum;
    else if(node.kind=="PcmOnsetSequence")plan.kind=PcmStageKind::Onset;
    else if(node.kind=="PcmContextTempo")plan.kind=PcmStageKind::Tempo;
    else if(node.kind=="ContextBeatTracker"){
        plan.kind=PcmStageKind::Tracker;return tracker_settings(node,plan.tracker,512);
    }else if(node.kind=="ContextBeatSequence"){
        plan.kind=PcmStageKind::Sequence;return sequence_settings(node,plan.sequence);
    }else return false;
    if(!spectrum_settings(node,plan))return false;
    if(plan.kind!=PcmStageKind::Tempo)return true;
    return sequence_settings(node,plan.tempo.sequence) && scalar_parameter(node,"threshold",.3f,plan.tempo.threshold) &&
        scalar_parameter(node,"silence_db",-90,plan.tempo.silence_db);
}
}
