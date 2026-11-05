// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/interpolated_peaks.h>
#include <dsp_kernels/numeric_rows.h>
#include <cstddef>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct WeightedTempoHistogramConfig {
    float minimum_bpm=30,maximum_bpm=560,tolerance_percent=3;
    bool weight_by_magnitude=true;
};
// Source BpmHistogram's global candidate ranking. Observation rows are
// [rounded_BPM,magnitude]; tempogram rows have ceil(maximum_bpm)+1 values.
// Readers are synchronous immutable views, never retained or cloned as full
// histories. The original ordered pair accumulation, deviation weighting,
// per-frame energy vote, tie order and .25 cutoff are retained.
// Output strengths are source-relative magnitudes, not probabilities.
class PreparedWeightedTempoHistogram {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t observations,std::size_t frames,WeightedTempoHistogramConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_observations,std::size_t maximum_frames,WeightedTempoHistogramConfig={});
    bool process(dsp_kernels::FloatMatrixSource observations,std::size_t observation_count,
                 dsp_kernels::FloatMatrixSource tempogram,std::size_t frames) noexcept;
    std::span<const float> histogram() const noexcept {return histogram_;}
    std::span<const InterpolatedPeak> candidates() const noexcept {return std::span(result_).first(count_);}
    bool silent() const noexcept {return silent_;}
private:
    bool read(dsp_kernels::FloatMatrixSource,std::size_t,InterpolatedPeak&) const noexcept;
    bool scan(dsp_kernels::FloatMatrixSource,std::size_t,float&) noexcept;
    float deviation_weight(float candidate,float center) const noexcept;
    bool accumulate(dsp_kernels::FloatMatrixSource,std::size_t,float maximum) noexcept;
    bool accumulate_at(dsp_kernels::FloatMatrixSource,std::size_t,float,const InterpolatedPeak&) noexcept;
    void seed() noexcept;
    bool enrich(dsp_kernels::FloatMatrixSource,std::size_t) noexcept;
    void add_frame_energy(float total) noexcept;
    void normalize() noexcept;
    WeightedTempoHistogramConfig config_;
    std::size_t maximum_observations_=0,maximum_frames_=0,count_=0;
    bool silent_=true;
    std::vector<float> histogram_,frame_;
    std::vector<InterpolatedPeak> result_;
};
}
