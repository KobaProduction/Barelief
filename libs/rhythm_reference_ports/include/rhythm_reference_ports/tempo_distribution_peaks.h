// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/interpolated_peaks.h>
#include <array>
#include <span>

namespace rhythm_reference_ports {
// Source NoveltyCurveFixedBpmEstimator's histogramPeaks: 100 data-range bins,
// four zero tail bins, range104 interpolation, then rounded centre lookup.
// The historical range/array-length mismatch is intentionally preserved.
// Approximate BPM merging remains an explicit independent operation.
class PreparedTempoDistributionPeaks {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare();
    bool process(std::span<const float> bpms) noexcept;
    bool merge(float tolerance_percent) noexcept;
    void sort_by_magnitude() noexcept;
    std::span<const InterpolatedPeak> peaks() const noexcept {return std::span(peaks_).first(count_);}
    std::span<const float> histogram() const noexcept {return histogram_;}
    std::span<const float> centers() const noexcept {return centers_;}
private:
    bool histogram(std::span<const float>) noexcept;
    bool map_peaks() noexcept;
    bool merge_pair(std::size_t left,std::size_t right,float) noexcept;
    PreparedInterpolatedPeaks detector_;
    std::size_t count_=0;
    bool prepared_=false;
    std::array<float,104> histogram_{},centers_{};
    std::array<InterpolatedPeak,104> peaks_{};
};
}
