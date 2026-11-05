// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/onset_recipes.h>
#include <cstddef>
#include <span>
namespace rhythm_reference_ports {
// Common source descriptor dispatcher over existing spectral/histogram kernels.
// Values are already preprocessed magnitudes with original FFT phase. No
// whitening, compression, picker or clocks are hidden; the caller composes them.
class PreparedOnsetDescriptor {
public:
    bool prepare(std::size_t bins,OnsetRecipe);
    // Pure dynamic-payload bound; excludes this object and caller stack.
    static bool storage_bytes(std::size_t bins,OnsetRecipe,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    // Synchronous views after successful process; only histogram recipes have
    // these rows. Invalidated by the next process/restart/prepare/destruction.
    std::span<const float> histogram_counts() const noexcept {
        return recipe_.kind==OnsetDescriptorKind::Histogram?histogram_.histogram_counts():std::span<const float>{};
    }
    std::span<const float> rescaled_histogram_values() const noexcept {
        return recipe_.kind==OnsetDescriptorKind::Histogram?histogram_.rescaled_histogram_values():std::span<const float>{};
    }
    void restart() noexcept;
    bool process(std::span<const float> magnitude,std::span<const float> phase,float&) noexcept;
private:
    OnsetDescriptorRecipe recipe_;
    dsp_kernels::PreparedSpectralDescriptor spectral_;
    PreparedHistogramOnset histogram_;
    bool prepared_=false;
};
}
