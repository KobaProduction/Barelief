// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/buffered_maximum.h>
#include <rhythm_reference_ports/causal_average.h>
#include <cstdint>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct ContrastPeakPickerConfig {
    float frame_rate=172,threshold=.05f,ratio_threshold=16;
    float combine_ms=30,pre_average_ms=100,pre_maximum_ms=30;
};
// Source SuperFluxPeaks standard numerical stages. Causal average/maximum
// states and source Float32 batch start time persist. Combine applies only
// WITHIN a call, with strict >; the streaming boundary rule lives separately.
// The source reads signal[size] and may write an unallocated output vector;
// only real samples are processed, with prepared output storage (safe repair).
class PreparedContrastPeakPicker {
public:
    bool prepare(std::size_t maximum_samples,ContrastPeakPickerConfig={});
    static bool storage_bytes(std::size_t maximum_samples,ContrastPeakPickerConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    void reset(bool clear_detection_counter=false) noexcept;
    bool process(std::span<const float> novelty) noexcept;
    std::span<const float> onsets() const noexcept {return std::span(onsets_).first(count_);}
    std::span<const float> averages(std::size_t count) const noexcept;
    std::span<const float> maxima(std::size_t count) const noexcept;
    std::uint64_t detections() const noexcept {return detections_;}
private:
    bool configure(std::size_t,ContrastPeakPickerConfig);
    bool prepare_filters(ContrastPeakPickerConfig);
    bool over_threshold(float,float) const noexcept;
    bool scan(std::span<const float>) noexcept;
    ContrastPeakPickerConfig config_;
    float start_time_=0,combine_seconds_=0;
    std::size_t count_=0,processed_=0;
    std::uint64_t detections_=0;
    PreparedBufferedMaximum maximum_;
    PreparedCausalAverage average_;
    std::vector<float> averages_,maxima_,onsets_;
};
// Source streaming accumulator, including its historical boundary expression:
// trim uses current_t+batch[0]-last, and on trim it removes the LAST batch
// event, or the whole batch if exactly one previous event exists. These quirks
// are kept for source comparison, not replaced with a different combine rule.
// Capacity is explicit; no event silently disappears on memory exhaustion.
class PreparedContrastPeakAccumulator {
public:
    bool prepare(std::size_t maximum_chunk_samples,std::size_t maximum_onsets,ContrastPeakPickerConfig={});
    static bool storage_bytes(std::size_t maximum_chunk_samples,std::size_t maximum_onsets,ContrastPeakPickerConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return picker_.workspace_bytes()+onsets_.capacity()*sizeof(float);}
    bool consume(std::span<const float>) noexcept;
    bool finish(std::span<float> output,std::size_t& count) noexcept;
    void reset() noexcept;
    std::span<const float> onsets() const noexcept {return std::span(onsets_).first(count_);}
private:
    bool append(std::span<const float>) noexcept;
    PreparedContrastPeakPicker picker_;
    float frame_rate_=0,combine_seconds_=0,current_time_=0;
    std::size_t count_=0;
    std::vector<float> onsets_;
};
}
