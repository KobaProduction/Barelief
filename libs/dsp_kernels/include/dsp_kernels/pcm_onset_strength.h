// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/onset_strength_sequence.h>
#include <dsp_kernels/spectral_frame_recipe.h>
#include <dsp_kernels/power_to_db.h>
namespace dsp_kernels {
// Source default PCM -> power-Mel settings with explicit Fs/hop/FFT.
// Reuses the general frontend; no source-library name is part of its contract.
SpectralFrameRecipeConfig mel_power_spectral_parameters(float sample_rate=44100,
    std::size_t fft_size=2048,std::size_t hop_samples=512) noexcept;
struct PcmOnsetStrengthConfig {
    SpectralFrameRecipeConfig spectrum=mel_power_spectral_parameters();
    PowerToDbConfig decibels;
    OnsetStrengthSequenceConfig onset;
};
PcmOnsetStrengthConfig median_onset_parameters() noexcept;
// PCM -> existing spectral/window/FFT/Mel -> measured dB -> shared onset
// sequence. No whole spectrogram/history clone: the reader recomputes requested
// frames against one measured matrix's scales. PCM is borrowed only in process.
// Batch channels are independent and reuse prepared storage sequentially.
class PreparedPcmOnsetStrength {
public:
    static bool storage_bytes(std::size_t,PcmOnsetStrengthConfig,std::span<const float>,ExplicitSpectralFilters,
        std::span<const OnsetFeatureGroup>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_samples,PcmOnsetStrengthConfig={},
        std::span<const float> window={},ExplicitSpectralFilters={},
        std::span<const OnsetFeatureGroup> groups={},OnsetAggregationPort={});
    std::size_t frames(std::size_t samples) const noexcept {return onset_.output_frames(spectrum_.frames(samples));}
    std::size_t output_features() const noexcept {return onset_.output_features();}
    bool process(std::span<const float> pcm,FloatMatrixDestination,FloatMatrixSource reference={}) noexcept;
    bool process_channels(std::span<const std::span<const float>> channels,
        std::span<const FloatMatrixDestination> outputs) noexcept;
private:
    struct Rows {PreparedPcmOnsetStrength* owner;std::span<const float> pcm;};
    static bool read_power(const void*,std::size_t,std::span<float>) noexcept;
    static bool read_db(const void*,std::size_t,std::span<float>) noexcept;
    PreparedSpectralFrameRecipe spectrum_;
    PreparedPowerToDb decibels_;
    PreparedOnsetStrengthSequence onset_;
    std::size_t maximum_samples_=0,features_=0;
};
}
