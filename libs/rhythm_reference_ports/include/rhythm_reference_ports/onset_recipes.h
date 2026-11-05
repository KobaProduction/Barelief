// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/onset_decision.h>
#include <rhythm_reference_ports/histogram_onsets.h>
#include <dsp_kernels/energy_novelty.h>
#include <dsp_kernels/spectral_descriptors.h>
namespace rhythm_reference_ports {
enum class OnsetDescriptorKind {Spectral,Histogram};
struct OnsetDescriptorRecipe {
    OnsetDescriptorKind kind=OnsetDescriptorKind::Spectral;
    dsp_kernels::SpectralDescriptorConfig spectral;
    HistogramOnsetConfig histogram;
};
// Aubio's nine onset descriptors (plus old-default alias), independent from
// its whitening/compression/peak/decision presets. Energy here sums SPECTRAL
// magnitudes squared; phase/specdiff select the histogram operation.
bool onset_descriptor_recipe(OnsetRecipe,OnsetDescriptorRecipe&) noexcept;
// Source truncating conversions from explicit PCM rate/hop to sample controls.
bool onset_decision_parameters(OnsetRecipe,std::uint32_t hop_samples,float sample_rate,
                               OnsetDecisionConfig&) noexcept;
enum class PredictiveOnsetMethod {
    EnergyEnvelope,EnergyDifference,SpectralDifference,PositiveSpectralDifference,
    PhaseDeviation,ComplexDifference,PositiveComplexDifference,FrequencyContent,
    FrequencyDifference,PositiveFrequencyDifference
};
struct PredictiveOnsetParameters {
    bool uses_pcm_energy=false;
    dsp_kernels::EnergyNoveltyConfig energy;
    dsp_kernels::SpectralDescriptorConfig spectral;
};
// BTrack's ten source ODFs. Energy consumes a complete UNWINDOWED PCM frame;
// spectral modes consume raw complex FFT, with source full-spectrum symmetry.
// FFT window/half-rotation remain separate operations. The spectral source
// requires even frame sizes; energy does not need an FFT size.
bool predictive_onset_parameters(PredictiveOnsetMethod,std::size_t fft_size,
                                 PredictiveOnsetParameters&) noexcept;
}
