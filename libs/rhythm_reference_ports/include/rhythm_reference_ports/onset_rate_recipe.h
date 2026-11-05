// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numerical_precision.h>
#include <rhythm_reference_ports/onset_events.h>
#include <rhythm_reference_ports/polar_novelty.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <dsp_kernels/tempogram.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
enum class OnsetRateRecipe {Standard,Streaming};
// Full source OnsetRate PCM recipe (fixed Fs=44100, frame=1024, hop=512).
// Standard: uncentered CoverEnd, rate denominator=PCM samples/Fs.
// Streaming: centered CenterCoverEnd, silence kept, denominator=ODF frames*hop/Fs.
// Both use normalized symmetric Hann, zero-phase raw FFT, Brossier HFC and
// source complex ODF, then existing Onsets fusion/FIR/selection with [1,1].
// process() evaluates one complete independent batch, equivalent to source
// reset + compute/end-of-input; streaming chunk transport belongs to runtime.
// Feature histories are needed for source global normalization; PCM is borrowed.
class PreparedOnsetRateRecipe {
public:
    bool prepare(std::size_t maximum_pcm_samples,OnsetRateRecipe=OnsetRateRecipe::Standard,
                 dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32,const dsp_kernels::FourierBackendFactory* =nullptr);
    static bool storage_bytes(std::size_t maximum_pcm_samples,OnsetRateRecipe,std::size_t&,
                              dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    void reset() noexcept;
    bool process(std::span<const float>) noexcept;
    float rate() const noexcept {return ready_?rate_:0;}
    float duration_seconds() const noexcept {return ready_?duration_:0;}
    std::span<const float> onset_times() const noexcept;
    std::span<const float> hfc_curve() const noexcept;
    std::span<const float> complex_curve() const noexcept;
    const PreparedOnsetEvents& events() const noexcept {return events_;}
private:
    bool configure(std::size_t,OnsetRateRecipe,dsp_kernels::NumericalPrecision,const dsp_kernels::FourierBackendFactory*);
    bool prepare_spectrum(OnsetRateRecipe,dsp_kernels::NumericalPrecision,const dsp_kernels::FourierBackendFactory*);
    bool prepare_features(std::size_t);
    bool frame(std::span<const float>,std::size_t) noexcept;
    bool select(std::size_t) noexcept;
    static bool read_feature(const void*,std::size_t,std::span<float>) noexcept;
    std::size_t maximum_pcm_=0,frame_count_=0,onset_count_=0;
    OnsetRateRecipe recipe_=OnsetRateRecipe::Standard;
    bool ready_=false;
    float rate_=0,duration_=0;
    dsp_kernels::PreparedFourierTempogram spectrum_;
    dsp_kernels::PreparedSpectralDescriptor hfc_;
    PreparedPolarNovelty complex_novelty_;
    PreparedOnsetEvents events_;
    std::vector<float> complex_,magnitude_,phase_,distances_,hfc_curve_,complex_curve_,times_;
    std::vector<std::uint8_t> decisions_;
};
}
