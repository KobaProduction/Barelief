// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/recursive_filter.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <cstddef>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct PulseOnsetObservation {float novelty=0,filtered=0;};
// Source Percival's magnitude -> /N -> log(1+1000*x) -> positive L1 flux
// -> original 15-tap FIR. The explicit upstream PCM window is unnormalized
// symmetric Hamming, without zero-phase rotation. FFT/window are shared blocks.
// The source FIR coefficients stay fixed even when Fs/hop change; no implicit
// redesign. Both pre-filter and filtered novelty are independently observable.
class PreparedPulseOnset {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t bins,std::size_t fft_size,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t spectrum_bins,std::size_t source_fft_size);
    void reset() noexcept;
    bool process(std::span<const float> magnitude,PulseOnsetObservation&) noexcept;
    std::span<const float> compressed_spectrum() const noexcept {return compressed_;}
private:
    bool configure(std::size_t,std::size_t);
    bool compress(std::span<const float>) noexcept;
    dsp_kernels::PreparedSpectralDescriptor difference_;
    dsp_kernels::PreparedRecursiveFilter filter_;
    std::size_t fft_size_=0;
    std::vector<float> scaled_,compressed_;
};
}
