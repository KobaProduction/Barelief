// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "parameter_values.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
namespace {
bool flag(const domain::ResolvedProcessingNode& node,std::string_view key,bool fallback,bool& output) noexcept {
    const auto* item=parameter(node,key);if(!item){output=fallback;return true;}
    const auto* value=std::get_if<bool>(item);if(!value)return false;output=*value;return true;
}
bool selection_settings(const domain::ResolvedProcessingNode& node,AdaptiveOnsetSelectionConfig& c) noexcept {
    return integer_parameter(node,"delay",5,c.delay) && scalar_parameter(node,"frame_rate",44100.f/512.f,c.frame_rate) &&
        scalar_parameter(node,"alpha",.1f,c.alpha) && scalar_parameter(node,"silence_threshold",.02f,c.silence_threshold);
}
bool activation_settings(const domain::ResolvedProcessingNode& n,ActivationPeakSequenceConfig& c) noexcept {
    return scalar_parameter(n,"frame_rate",100,c.frame_rate) && scalar_parameter(n,"threshold",.5f,c.threshold) &&
        scalar_parameter(n,"smooth",0,c.smooth) && scalar_parameter(n,"pre_average",0,c.pre_average) && scalar_parameter(n,"post_average",0,c.post_average) &&
        scalar_parameter(n,"pre_maximum",0,c.pre_maximum) && scalar_parameter(n,"post_maximum",0,c.post_maximum) &&
        scalar_parameter(n,"combine",.03f,c.combine) && scalar_parameter(n,"delay",0,c.delay) && flag(n,"online",false,c.online);
}
bool difference_settings(const domain::ResolvedProcessingNode& n,SpectralDifferenceSequenceConfig& c) noexcept {
    return integer_parameter(n,"lag",c.lag,c.lag) && integer_parameter(n,"frequency_maximum",c.frequency_maximum,c.frequency_maximum) && flag(n,"positive",c.positive,c.positive);
}
bool pcm_difference_settings(const domain::ResolvedProcessingNode& n,PcmSpectralDifferenceConfig& c) noexcept {
    float hop=441,rate=0;std::size_t window=0;
    if(!integer_parameter(n,"frame_samples",2048,c.spectrum.framing.length) || !integer_parameter(n,"fft_size",0,c.spectrum.fft_size) ||
       !scalar_parameter(n,"hop_samples",441,hop) || !scalar_parameter(n,"sample_rate",44100,c.spectrum.sample_rate) ||
       !scalar_parameter(n,"frame_rate",0,rate) || !integer_parameter(n,"window",0,window) || window>6 ||
       !flag(n,"periodic",false,c.spectrum.periodic) || !flag(n,"automatic_lag",true,c.automatic_lag) ||
       !scalar_parameter(n,"overlap_ratio",.5f,c.overlap_ratio) || !difference_settings(n,c.difference))return false;
    c.spectrum.framing.hop_samples=hop;c.spectrum.frame_rate=rate;c.spectrum.window=dsp_kernels::WindowFunction(window);
    return true;
}
bool trajectory_settings(const domain::ResolvedProcessingNode& n,TrajectoryOnsetRecipeConfig& c) noexcept {
    return integer_parameter(n,"frame_size",2048,c.frame_size) && integer_parameter(n,"audio_hop",256,c.audio_hop) &&
        integer_parameter(n,"novelty_chunk_size",4096,c.novelty_chunk_size) && scalar_parameter(n,"sample_rate",44100,c.sample_rate) &&
        scalar_parameter(n,"band_sample_rate",44100,c.band_sample_rate) && scalar_parameter(n,"threshold",.05f,c.threshold) &&
        scalar_parameter(n,"ratio_threshold",16,c.ratio_threshold) && scalar_parameter(n,"combine_ms",20,c.combine_ms);
}
}
bool sequence_source_settings(const domain::ResolvedProcessingNode& n,SequenceSourcePlan& p) noexcept {
    if(!flag(n,"reset_each_call",true,p.reset_each_call))return false;
    if(p.kind==SequenceSourceKind::Fusion)return true;
    if(p.kind==SequenceSourceKind::Selection || p.kind==SequenceSourceKind::Events)return selection_settings(n,p.selection);
    if(p.kind==SequenceSourceKind::Activation)return activation_settings(n,p.activation);
    if(p.kind==SequenceSourceKind::Difference)return difference_settings(n,p.difference);
    if(p.kind==SequenceSourceKind::PcmDifference)return pcm_difference_settings(n,p.pcm_difference);
    if(p.kind==SequenceSourceKind::Trajectory)return integer_parameter(n,"maximum_onsets",4096,p.events) && trajectory_settings(n,p.trajectory);
    if(p.kind==SequenceSourceKind::IntervalSummary)return scalar_parameter(n,"maximum_bpm",1000,p.maximum_bpm);
    std::size_t method=0;if(!integer_parameter(n,"method",p.kind==SequenceSourceKind::PcmSummary?1:0,method) || method>1)return false;
    if(p.kind==SequenceSourceKind::Rate){p.rate=OnsetRateRecipe(method);return true;}
    p.beat.method=PcmBeatMethod(method);std::size_t minimum=40,maximum=208;
    if(!integer_parameter(n,"minimum_bpm",40,minimum) || !integer_parameter(n,"maximum_bpm",208,maximum) || minimum>INT32_MAX || maximum>INT32_MAX)return false;
    p.beat.minimum_bpm=int(minimum);p.beat.maximum_bpm=int(maximum);return true;
}
}
