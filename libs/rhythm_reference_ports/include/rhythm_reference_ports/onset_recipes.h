// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/onset_decision.h>
#include <rhythm_reference_ports/histogram_onsets.h>
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
}
