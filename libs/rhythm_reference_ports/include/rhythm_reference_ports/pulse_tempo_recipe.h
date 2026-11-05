// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numerical_precision.h>
#include <rhythm_reference_ports/pulse_onset.h>
#include <rhythm_reference_ports/pulse_correlation.h>
#include <rhythm_reference_ports/lag_tempo_consensus.h>
#include <rhythm_reference_ports/scalar_history.h>
#include <dsp_kernels/tempogram.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct PulseTempoRecipeConfig {
    std::size_t frame_size=1024,audio_hop=128,onset_window=2048,onset_hop=128;
    int sample_rate=44100,minimum_bpm=50,maximum_bpm=210;
    dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32;
};
// Complete source PCM->Percival numerical recipe, as independently reset batch
// analysis. Complete uncentered PCM frames, unnormalized symmetric Hamming,
// raw Fourier/magnitude, source compression/flux/FIR, uncentered OSS frames
// including the first reaching EOF, generalized ACF/pulse selection and
// Gaussian/SVM octave decision. No alternate tracker or new executor.
// A mirrored ScalarHistory exposes complete OSS windows without cloning.
// Only the final partial OSS frame is explicitly padded by the shared temporal
// window operation. Lags feed consensus in original order, without a full pool.
class PreparedPulseTempoRecipe {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t,PulseTempoRecipeConfig,TempoOctaveClassifier,std::size_t&,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_pcm_samples,PulseTempoRecipeConfig={},TempoOctaveClassifier={},const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process(std::span<const float> pcm) noexcept;
    void reset() noexcept;
    const LagTempoConsensusResult& result() const noexcept {return result_;}
    std::uint64_t lag_observations() const noexcept {return consensus_.observations();}
    // Last successful audio/OSS stages, not a cloned full spectrogram.
    std::size_t processed_audio_frames() const noexcept {return diagnostic_frames_;}
    dsp_kernels::FourierTempogramView spectral_frame() const noexcept {return diagnostic_frames_?spectrum_.view():dsp_kernels::FourierTempogramView{};}
    std::span<const float> last_magnitude() const noexcept {return diagnostic_frames_?std::span<const float>(magnitude_):std::span<const float>{};}
    std::span<const float> last_compressed() const noexcept {return diagnostic_frames_?onset_.compressed_spectrum():std::span<const float>{};}
    std::span<const float> lag_histogram() const noexcept {return consensus_.histogram();}
    std::span<const float> last_correlation() const noexcept {return analyzed_?correlation_.correlation():std::span<const float>{};}
    std::span<const float> last_enhanced() const noexcept {return analyzed_?correlation_.enhanced():std::span<const float>{};}
private:
    bool configure(std::size_t,PulseTempoRecipeConfig,TempoOctaveClassifier,const dsp_kernels::FourierBackendFactory*);
    bool prepare_spectrum(PulseTempoRecipeConfig,const dsp_kernels::FourierBackendFactory*);
    bool prepare_onset_frames(PulseTempoRecipeConfig,TempoOctaveClassifier,const dsp_kernels::FourierBackendFactory*);
    bool audio_frame(std::span<const float>,std::size_t) noexcept;
    bool onset_frame(float) noexcept;
    bool analyze(std::span<const float>) noexcept;
    bool finish_tail() noexcept;
    PulseTempoRecipeConfig config_;
    std::size_t maximum_pcm_=0,onset_count_=0,analyzed_=0,next_end_=0;
    std::size_t diagnostic_frames_=0;
    dsp_kernels::PreparedFourierTempogram spectrum_;
    dsp_kernels::PreparedTemporalWindow onset_window_;
    PreparedPulseOnset onset_;
    PreparedPulseCorrelation correlation_;
    PreparedLagTempoConsensus consensus_;
    ScalarHistory history_;
    LagTempoConsensusResult result_;
    std::vector<float> complex_,magnitude_,tail_;
};
}
