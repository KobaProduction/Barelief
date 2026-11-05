// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/fourier_tempo_candidates.h>
#include <rhythm_reference_ports/weighted_tempo_histogram.h>
#include <rhythm_reference_ports/frame_tempo_path.h>
#include <rhythm_reference_ports/phase_tempo_pulse.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct FourierTempoHistogramConfig {
    FrameTempoPathConfig path;
    std::size_t maximum_peaks=50;
    bool weight_by_magnitude=true;
};
// Numerical composition of the source BpmHistogram after FFT/polar conversion.
// Explicit magnitude and phase rows include DC/Nyquist; frequency_step belongs
// to the novelty transform. Frame cutting, windowing, zero-phase rotation and
// FFT remain upstream operations, not hidden PCM assumptions in this class.
// Only derived raw peaks, BPM observations and output tempogram are stored;
// complete borrowed magnitude/phase histories are never copied or retained.
// No executor, I/O or UI. Runtime adapters schedule this like other kernels.
class PreparedFourierTempoHistogram {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t frames,std::size_t bins,std::span<const float> window,FourierTempoHistogramConfig,std::size_t&) noexcept;
    static bool storage_bytes(std::size_t frames,std::size_t bins,std::size_t window_length,FourierTempoHistogramConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,std::size_t bins,
                 std::span<const float> synthesis_window,FourierTempoHistogramConfig={});
    bool process(dsp_kernels::FloatMatrixSource magnitude,dsp_kernels::FloatMatrixSource phase,
                 std::size_t frames) noexcept;
    void reset(bool reset_reference=false) noexcept;
    float bpm() const noexcept {return result_bpm_;}
    FrameTempoPathStatus status() const noexcept {return status_;}
    std::span<const InterpolatedPeak> candidates() const noexcept;
    std::span<const float> frame_bpms() const noexcept;
    std::span<const float> tempogram_frame(std::size_t) const noexcept;
    std::span<const float> pulse() const noexcept;
    std::span<const InterpolatedPeak> ticks() const noexcept;
    std::size_t frames() const noexcept {return visible_frames_;}
    std::size_t collected_frames() const noexcept {return input_frames_;}
    std::span<const float> analysis_tempogram_frame(std::size_t frame) const noexcept;
    std::span<const InterpolatedPeak> raw_peaks(std::size_t frame) const noexcept {return read_peaks(this,frame);}
    std::span<const InterpolatedPeak> observations() const noexcept {return std::span(observations_).first(observation_count_);}
    std::span<const float> histogram() const noexcept {return ranking_.histogram();}
private:
    bool configure(std::size_t,std::size_t,std::span<const float>,FourierTempoHistogramConfig);
    bool prepare_stages(std::size_t,std::size_t,std::span<const float>);
    void allocate(std::size_t,std::size_t);
    void clear_outputs() noexcept;
    bool collect(dsp_kernels::FloatMatrixSource,std::size_t) noexcept;
    bool collect_frame(dsp_kernels::FloatMatrixSource,std::size_t) noexcept;
    bool finish(dsp_kernels::FloatMatrixSource,std::size_t) noexcept;
    static bool read_observation(const void*,std::size_t,std::span<float>) noexcept;
    static bool read_tempogram(const void*,std::size_t,std::span<float>) noexcept;
    static std::span<const InterpolatedPeak> read_peaks(const void*,std::size_t) noexcept;
    FourierTempoHistogramConfig config_;
    PreparedFourierTempoCandidates per_frame_;
    PreparedWeightedTempoHistogram ranking_;
    PreparedFrameTempoPath path_;
    PreparedPhaseTempoPulse pulse_;
    std::size_t maximum_frames_=0,tempo_bins_=0,visible_frames_=0,input_frames_=0;
    std::size_t peak_count_=0,observation_count_=0;
    float result_bpm_=0;
    FrameTempoPathStatus status_=FrameTempoPathStatus::Unprocessed;
    std::vector<float> magnitude_,tempogram_;
    std::vector<std::size_t> peak_offsets_;
    std::vector<InterpolatedPeak> raw_peaks_,observations_;
};
}
