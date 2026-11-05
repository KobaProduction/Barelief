// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/interpolated_peaks.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct FourierTempoCandidateConfig {
    float frequency_step=1,minimum_bpm=30,maximum_bpm=560;
    std::size_t maximum_peaks=50;
};
// Source BpmHistogram's per-spectrum candidate stage. Input is a complete raw
// magnitude row from an explicitly windowed/zero-phase FFT. frequency_step is
// Hz per bin of the novelty-series FFT, not PCM Fs. Keeps original spectral
// peaks separately from rounded BPM observations. Collision bins overwrite as
// in the source. An empty candidate frame emits the source {BPM0,amplitude0}.
class PreparedFourierTempoCandidates {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t bins,FourierTempoCandidateConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t bins,FourierTempoCandidateConfig);
    bool process(std::span<const float> magnitude) noexcept;
    std::span<const InterpolatedPeak> spectral_peaks() const noexcept {return detector_.peaks();}
    std::span<const InterpolatedPeak> bpm_observations() const noexcept {return std::span(observations_).first(count_);}
    std::span<const float> tempo_row() const noexcept {return tempo_;}
private:
    bool configure(std::size_t,FourierTempoCandidateConfig);
    bool prepare_detector();
    float threshold(std::span<const float>) noexcept;
    FourierTempoCandidateConfig config_;
    PreparedInterpolatedPeaks detector_;
    std::size_t bins_=0,count_=0;
    std::vector<float> scratch_,tempo_;
    std::vector<InterpolatedPeak> observations_;
};
}
