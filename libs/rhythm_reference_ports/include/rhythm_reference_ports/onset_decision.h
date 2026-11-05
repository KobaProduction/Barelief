// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/spectral_whitening.h>
#include <cstdint>
#include <span>
namespace rhythm_reference_ports {
struct OnsetDecisionConfig {
    std::uint32_t hop_samples=512;
    std::uint64_t minimum_interval_samples=2205,delay_samples=2201;
};
struct OnsetDecision {
    float peak_position=0;
    std::uint64_t sample=0;
    bool detected=false;
};
// Source aubio onset acceptance after its independent adaptive peak picker.
// Silence is an explicit boolean input; counters are sample coordinates.
// Retains strict min-IOI, delay compensation and special startup behavior.
// 64-bit counters extend the source's 32-bit clock without wraparound.
class PreparedOnsetDecision {
public:
    bool prepare(OnsetDecisionConfig={}) noexcept;
    void reset() noexcept;
    bool process(float peak_position,bool silent,OnsetDecision&) noexcept;
    bool set_delay_samples(std::uint64_t) noexcept;
    bool set_minimum_interval_samples(std::uint64_t) noexcept;
    std::uint64_t delay_samples() const noexcept {return config_.delay_samples;}
    std::uint64_t minimum_interval_samples() const noexcept {return config_.minimum_interval_samples;}
    std::uint64_t last_sample() const noexcept {return last_>=config_.delay_samples?last_-config_.delay_samples:0;}
private:
    bool candidate(float,bool,float&) noexcept;
    void startup(bool,float&) noexcept;
    bool separated(std::uint64_t sample) const noexcept;
    OnsetDecisionConfig config_;
    std::uint64_t total_=0,last_=0;
    bool prepared_=false;
};
bool pcm_is_silent(std::span<const float> pcm,float threshold_db,bool& silent) noexcept;
enum class OnsetRecipe {Energy,FrequencyContent,Complex,Phase,WeightedPhase,
                        ModifiedKullbackLeibler,KullbackLeibler,SpectralFlux,SpectralDifference,OldDefault};
struct OnsetRecipeParameters {
    float threshold=.3f,compression=0,delay_hops=4.3f,minimum_interval_s=.05f,silence_db=-70;
    bool whitening=false;
    SpectralWhiteningConfig whitening_config;
};
// Source numerical presets only. Framing/FFT, descriptor, whitening,
// compression, peak picker and decision remain separate explicit components.
OnsetRecipeParameters onset_recipe_parameters(OnsetRecipe) noexcept;
}
