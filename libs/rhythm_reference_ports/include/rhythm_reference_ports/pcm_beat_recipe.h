// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numerical_precision.h>
#include <rhythm_reference_ports/spectral_onset.h>
#include <rhythm_reference_ports/global_onset_recipe.h>
#include <rhythm_reference_ports/multi_feature_beat_tracker.h>
#include <dsp_kernels/tempogram.h>
#include <array>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
enum class PcmBeatMethod {SingleComplex,MultiFeature};
struct PcmBeatRecipeConfig {
    PcmBeatMethod method=PcmBeatMethod::MultiFeature;
    int minimum_bpm=40,maximum_bpm=208;
    UniformNoisePort noise;
    dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32;
};
struct PcmBeatFrameStages {
    SpectralFrameStages spectrum;
    // SingleComplex: one value. MultiFeature: Complex, RMS, MelFlux in order.
    std::span<const float> onset_values;
};
struct PcmBeatStageOutputs {
    FrameStageOutputPort<PcmBeatFrameStages> spectral;
    FrameStageOutputPort<GlobalOnsetFrameStages> emphasis,information_gain;
};
// Full BeatTrackerDegara / BeatTrackerMultiFeature PCM numerical recipes.
// Source fixed Fs=44100; 2048/1024 uncentered Hann complex/RMS/MelFlux share one
// FFT per frame, then x2 ODF interpolation in the existing candidate tracker.
// MultiFeature adds 2048/512 global beat-emphasis and information-gain curves,
// no ODF interpolation there, then the existing max-agreement selection.
// Confidence retains the source int-log2(40)=5 minus phase entropy, without
// probability scaling or clamping. Source comments describing 5.32 are not
// the actual integer-template behavior of the pinned implementation.
// process() is an independent source reset + complete PCM/end-of-input batch.
// No separate executor or PCM storage. All curves/intermediate candidates can
// be inspected; graph/chunk input transport is an explicit runtime concern.
// Synchronous stage outputs run in source order: primary spectral frames,
// then all emphasis frames, then all information-gain frames, then tracking.
// SpectralFrameStages uses the actual normalized window coefficients and the
// existing Fourier/polar primitives; it does not substitute another frontend.
// Source: https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/rhythm/beattrackermultifeature.cpp#L139-L199
class PreparedPcmBeatRecipe {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_pcm_samples,PcmBeatRecipeConfig,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_pcm_samples,PcmBeatRecipeConfig={},const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process(std::span<const float>,PcmBeatStageOutputs outputs={}) noexcept;
    PcmBeatFrameStages frame_stages() const noexcept;
    GlobalOnsetFrameStages global_frame_stages(std::size_t method) const noexcept;
    std::span<const float> ticks() const noexcept;
    std::span<const float> onset_curve(std::size_t method) const noexcept;
    std::span<const float> candidate_times(std::size_t method) const noexcept;
    std::span<const float> method_scores() const noexcept;
    float confidence() const noexcept;
    std::size_t maximum_tick_count() const noexcept {return maximum_ticks_;}
private:
    bool prepare_spectral_features(std::size_t,PcmBeatRecipeConfig);
    bool prepare_single_tracker(std::size_t,PcmBeatRecipeConfig);
    bool process_spectral_frames(std::span<const float>,FrameStageOutputPort<PcmBeatFrameStages>) noexcept;
    bool configure(std::size_t,PcmBeatRecipeConfig,const dsp_kernels::FourierBackendFactory*);
    bool prepare_spectrum(std::size_t,PcmBeatRecipeConfig,const dsp_kernels::FourierBackendFactory*);
    bool prepare_global(std::size_t,PcmBeatRecipeConfig,const dsp_kernels::FourierBackendFactory*);
    bool prepare_tracker(PcmBeatRecipeConfig);
    bool spectral_frame(std::span<const float>,std::size_t) noexcept;
    bool track_multi(std::span<const float>,PcmBeatStageOutputs) noexcept;
    bool track_single() noexcept;
    std::size_t maximum_pcm_=0,maximum_ticks_=0,frame_count_=0,tick_count_=0,frame_index_=0;
    PcmBeatMethod method_=PcmBeatMethod::MultiFeature;
    bool ready_=false,frame_ready_=false;
    dsp_kernels::PreparedFourierTempogram spectrum_;
    std::array<PreparedSpectralOnset,3> spectral_;
    PreparedGlobalOnsetRecipe emphasis_,gain_;
    PreparedPeriodBeatTracker single_;
    PreparedMultiFeatureBeatTracker multi_;
    std::array<std::vector<float>,3> curves_;
    std::array<float,3> frame_onsets_{};
    std::vector<float> complex_,magnitude_,phase_,ticks_;
    std::vector<std::uint8_t> mask_;
};
}
