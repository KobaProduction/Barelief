// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
// Experimental source Beatogram, retaining its "not evaluated" quality label.
// Input ratio rows are [beat][band]; output band_values() is explicitly
// [band][beat], matching the original transposed matrix, not a History layout.
// Per-tick and centered/clamped per-band max(mean,median) thresholds preserve
// source < versus <= conditions; surviving ratios multiply supplied energy.
// A sequence shorter than the configured window uses its complete extent,
// repairing the source negative iterator when nticks < window_size.
class PreparedBeatBandSalience {
public:
    // Checked owned dynamic cold-peak bound, excluding instance and allocator overhead.
    static bool storage_bytes(std::size_t beats,std::size_t bands,std::size_t window,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_beats,std::size_t maximum_bands,std::size_t window_size=16);
    bool process(dsp_kernels::FloatMatrixSource ratios,std::span<const float> energy,std::size_t bands) noexcept;
    std::span<const float> band_values(std::size_t) const noexcept;
    std::span<const float> tick_thresholds() const noexcept;
    std::size_t beats() const noexcept {return ready_?beats_:0;}
    std::size_t bands() const noexcept {return ready_?bands_:0;}
private:
    void allocate(std::size_t,std::size_t,std::size_t);
    bool band_thresholds(std::span<const float>) noexcept;
    bool filter_bands(std::span<const float>,std::size_t) noexcept;
    bool read(dsp_kernels::FloatMatrixSource,std::size_t,std::size_t) noexcept;
    bool filter_band(std::size_t,std::span<const float>) noexcept;
    float threshold(std::span<float>) noexcept;
    std::size_t maximum_beats_=0,maximum_bands_=0,window_size_=0,beats_=0,bands_=0;
    bool ready_=false;
    std::vector<float> values_,row_,scratch_,tick_thresholds_,band_thresholds_;
};
}
