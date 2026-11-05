// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct InterpolatedPeak {float position=0,value=0;};
enum class PeakOrdering {Position,Amplitude};
struct InterpolatedPeakConfig {
    float range=1,minimum=0,maximum=1,threshold=-1e6f,minimum_distance=0;
    std::size_t maximum_peaks=100;
    bool interpolate=true;
    PeakOrdering ordering=PeakOrdering::Position;
};
// Source PeakDetection's slope/plateau, boundary and strict distance semantics.
// Geometry/coefficients are explicit; all storage is prepared. Source invalid
// indexing for arrays of length two/outside-range starts is handled safely.
class PreparedInterpolatedPeaks {
public:
    bool prepare(std::size_t maximum_samples,InterpolatedPeakConfig={});
    // Pure dynamic-payload bound; sorting is in place, no heap scratch.
    static bool storage_bytes(std::size_t maximum_samples,InterpolatedPeakConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return peaks_.capacity()*sizeof(InterpolatedPeak);}
    bool process(std::span<const float> input) noexcept;
    bool set_threshold(float threshold) noexcept;
    bool set_coordinates(float range,float minimum,float maximum) noexcept;
    std::span<const InterpolatedPeak> peaks() const noexcept {return std::span(peaks_).first(count_);}
private:
    bool configure(std::size_t,InterpolatedPeakConfig);
    void collect(std::span<const float>,float scale) noexcept;
    bool walk(std::span<const float>,std::size_t&,float) noexcept;
    InterpolatedPeak candidate(std::span<const float>,std::size_t,std::size_t,float) const noexcept;
    void append(InterpolatedPeak) noexcept;
    void distance_filter() noexcept;
    bool separated(std::size_t,std::size_t) const noexcept;
    void order() noexcept;
    InterpolatedPeakConfig config_;
    std::size_t capacity_=0,count_=0;
    std::vector<InterpolatedPeak> peaks_;
};
}
