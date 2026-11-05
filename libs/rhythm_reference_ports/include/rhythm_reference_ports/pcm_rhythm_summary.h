// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/pcm_beat_recipe.h>
#include <rhythm_reference_ports/beat_interval_summary.h>
#include <span>

namespace rhythm_reference_ports {
// RhythmExtractor2013: full PCM tracker -> IBI -> two-BPM vote/mean, preserving
// original confidence (explicit zero for SingleComplex/Degara). Descriptors are
// the source RhythmDescriptors extension using the SAME IBI histogram kernel;
// its original no-parameter recipe uses the default MultiFeature configuration.
// No beat/PCM copies; views live until the next process or prepare call.
class PreparedPcmRhythmSummary {
public:
    bool prepare(std::size_t maximum_pcm_samples,PcmBeatRecipeConfig={},const dsp_kernels::FourierBackendFactory* =nullptr);
    static bool maximum_tick_count(std::size_t maximum_pcm_samples,PcmBeatRecipeConfig,std::size_t&) noexcept;
    static bool storage_bytes(std::size_t maximum_pcm_samples,PcmBeatRecipeConfig,std::size_t&,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept {return tracker_.workspace_bytes()+summary_.workspace_bytes();}
    bool process(std::span<const float>,PcmBeatStageOutputs={}) noexcept;
    const PreparedPcmBeatRecipe& tracker() const noexcept {return tracker_;}
    const PreparedBeatIntervalSummary& summary() const noexcept {return summary_;}
    bool histogram_descriptors(BeatIntervalHistogram&) const noexcept;
private:
    bool ready_=false;
    PreparedPcmBeatRecipe tracker_;
    PreparedBeatIntervalSummary summary_;
};
}
