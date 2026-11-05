// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numerical_precision.h>
#include <rhythm_reference_ports/global_onset.h>
#include <rhythm_reference_ports/polar_novelty.h>
#include <rhythm_reference_ports/erb_filter_bank.h>
#include <rhythm_reference_ports/frame_stage_outputs.h>
#include <dsp_kernels/tempogram.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
enum class GlobalOnsetMethod {InformationGain,BeatEmphasis};
struct GlobalOnsetRecipeConfig {
    float sample_rate=44100;
    std::size_t frame_size=2048,hop=512;
    GlobalOnsetMethod method=GlobalOnsetMethod::InformationGain;
    // Source omits forwarding PCM Fs into ERBBands. Changing this explicit
    // design Fs selects a variant; 44100 preserves its source default.
    float band_sample_rate=44100;
    dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32;
};
struct GlobalOnsetFrameStages {
    SpectralFrameStages spectrum;
    // InformationGain: selected source bins and the scalar emitted this frame.
    std::span<const float> information_magnitude;
    std::size_t information_first_bin=0;
    float information_gain=0;
    // BeatEmphasis: squared complex novelty distances -> magnitude ERB bank.
    // ERB centers/bin step belong to the FILTER DESIGN frequency axis, not
    // physical FFT bins when PCM Fs differs (physical axis is spectrum.Fs/N).
    std::span<const float> distances,erb_values,erb_centers_hz;
    float erb_sample_rate=0,erb_high_hz=0;
    double erb_bin_step_hz=0;
};
// Complete PCM OnsetDetectionGlobal numerical recipe. Uncentered source EOF
// framing / normalized symmetric Hann / zero-phase raw FFT. Information gain
// uses rounded 40..5000Hz bins and its unflushed 11-frame buffer. Beat emphasis
// uses source PLUS phase prediction, squared distances, 40 magnitude ERB bands,
// variance/threshold/FFT-ACF/comb weighting. Only the required band history
// lives in PreparedBandBeatEmphasis; a borrowed row reader avoids a second
// spectrogram. One owned 40-band row supports synchronous/last-frame outputs.
// ERB high bound follows PCM Fs/2; its design Fs is separately explicit and
// defaults to 44100, as the source omits forwarding. BeatEmphasis rejects
// PCM Fs above band_sample_rate; it never clamps the source high bound.
// One independent source reset + compute/end-of-input per process().
// Source: https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/rhythm/onsetdetectionglobal.cpp
class PreparedGlobalOnsetRecipe {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_pcm_samples,GlobalOnsetRecipeConfig,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_pcm_samples,GlobalOnsetRecipeConfig={},const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process(std::span<const float>,FrameStageOutputPort<GlobalOnsetFrameStages> output={}) noexcept;
    // Owned borrowed last-frame values; no DSP is run by this getter. Invalid
    // after the next process (including failure) or successful prepare. Outputs
    // are delivered per frame before global weighting, with no history clone.
    GlobalOnsetFrameStages frame_stages() const noexcept;
    std::size_t frames(std::size_t pcm_samples) const noexcept {return spectrum_.frames(pcm_samples);}
    std::span<const float> curve() const noexcept;
    std::span<const float> band_weights() const noexcept {return ready_?emphasis_.band_weights():std::span<const float>{};}
private:
    struct BorrowedPcm {PreparedGlobalOnsetRecipe* self;std::span<const float> pcm;FrameStageOutputPort<GlobalOnsetFrameStages> output;};
    bool configure(std::size_t,GlobalOnsetRecipeConfig,const dsp_kernels::FourierBackendFactory*);
    bool prepare_spectrum(GlobalOnsetRecipeConfig,const dsp_kernels::FourierBackendFactory*);
    bool prepare_gain(GlobalOnsetRecipeConfig);
    bool prepare_emphasis(std::size_t,GlobalOnsetRecipeConfig,const dsp_kernels::FourierBackendFactory*);
    bool spectrum(std::span<const float>,std::size_t) noexcept;
    bool information_gain(std::span<const float>,std::size_t,FrameStageOutputPort<GlobalOnsetFrameStages>) noexcept;
    bool erb_frame(std::span<const float>,std::size_t,std::span<float>,FrameStageOutputPort<GlobalOnsetFrameStages>) noexcept;
    static bool read_erb(const void*,std::size_t,std::span<float>) noexcept;
    std::size_t maximum_pcm_=0,count_=0,frame_index_=0,first_bin_=0,stop_bin_=0;
    bool ready_=false,frame_ready_=false;
    GlobalOnsetRecipeConfig config_;
    GlobalOnsetMethod method_=GlobalOnsetMethod::InformationGain;
    dsp_kernels::PreparedFourierTempogram spectrum_;
    PreparedInformationGain gain_;
    PreparedPolarNovelty novelty_;
    PreparedErbFilterBank erb_;
    PreparedBandBeatEmphasis emphasis_;
    std::vector<float> complex_,magnitude_,phase_,distances_,curve_,erb_values_;
};
}
