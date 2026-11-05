// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/causal_average.h>
#include <rhythm_reference_ports/interpolated_peaks.h>
#include <dsp_kernels/numeric_rows.h>
#include <dsp_kernels/overlap_add.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct PhaseTempoPulseConfig {
    float frame_rate=44100.f/512.f,frequency_step=1;
    std::size_t hop=32;
};
// Source BpmHistogram tick reconstruction from postprocessed frame BPMs and
// complete FFT phase rows. Explicit synthesis window is already max-normalized.
// Unwrap is along frequency bins, not time. Windowed cosine is clipped positive
// BEFORE raw OLA; no ISTFT denominator. Length is frames*hop (tail truncated).
// Fresh source MovingAverage per process, source fractional FIR/novelty delay,
// positional interpolated peaks and duplicate time-zero ticks are retained.
// Endpoint phase interpolation clamps to the last bin instead of source OOB.
// All workspaces are prepared; readers are borrowed only during process().
class PreparedPhaseTempoPulse {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t frames,std::size_t bins,std::span<const float> window,PhaseTempoPulseConfig,std::size_t&) noexcept;
    static bool storage_bytes(std::size_t frames,std::size_t bins,std::size_t window_length,PhaseTempoPulseConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,std::size_t phase_bins,
                 std::span<const float> synthesis_window,PhaseTempoPulseConfig={});
    bool process(dsp_kernels::FloatMatrixSource phases,std::span<const float> frame_bpms) noexcept;
    void reset() noexcept;
    std::span<const float> pulse() const noexcept {return std::span(pulse_).first(samples_);}
    std::span<const InterpolatedPeak> ticks() const noexcept {return std::span(ticks_).first(tick_count_);}
    std::span<const float> last_unwrapped_phase() const noexcept {return samples_?std::span<const float>(phase_):std::span<const float>{};}
    std::span<const float> last_synthesis_frame() const noexcept {return samples_ && diagnostic_frame_?std::span<const float>(frame_):std::span<const float>{};}
private:
    bool configure(std::size_t,std::size_t,std::span<const float>,PhaseTempoPulseConfig);
    bool prepare_filter_and_peaks(std::size_t);
    bool load_phase(dsp_kernels::FloatMatrixSource,std::size_t) noexcept;
    bool synthesize(std::size_t,float) noexcept;
    bool reconstruct(dsp_kernels::FloatMatrixSource,std::span<const float>) noexcept;
    bool smooth(std::span<const float>,float&) noexcept;
    bool detect(float delay) noexcept;
    PhaseTempoPulseConfig config_;
    std::size_t maximum_frames_=0,samples_=0,tick_count_=0;
    float bpm_ratio_=0,novelty_delay_=0;
    bool diagnostic_frame_=false;
    dsp_kernels::PreparedOverlapAdd overlap_;
    PreparedCausalAverage average_;
    PreparedInterpolatedPeaks detector_;
    std::vector<float> phase_,frame_,pulse_;
    std::vector<InterpolatedPeak> ticks_;
};
}
