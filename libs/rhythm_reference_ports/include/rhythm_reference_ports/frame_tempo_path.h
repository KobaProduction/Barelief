// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/interpolated_peaks.h>
#include <cstddef>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
// Borrowed, immutable per-frame raw spectral peaks in original amplitude order.
// No callback or span is retained after process().
struct SpectralPeakFrames {
    const void* context=nullptr;
    std::span<const InterpolatedPeak> (*read)(const void*,std::size_t) noexcept=nullptr;
};
struct FrameTempoPathConfig {
    float frequency_step=1,frame_rate=44100.f/512.f;
    float minimum_bpm=30,maximum_bpm=560,tolerance_percent=3;
    float minimum_change_seconds=5,induced_bpm=0;
    std::size_t hop=32;
    bool constant_tempo=false;
};
enum class FrameTempoPathStatus {Unprocessed,Ready,NoReference,NoMatchingTempo};
// Source raw-peak selection, octave folding and continuity. The source's
// boolean 0/1-percent continuity tolerance and j+1 skip are intentional here.
// The learned mean persists across calls, as in the original streaming object;
// reset() preserves the reference by default; reset(true) restores induction. No matching mean
// reports NoMatchingTempo instead of source division-by-zero/NaN.
class PreparedFrameTempoPath {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t frames,FrameTempoPathConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,FrameTempoPathConfig={});
    void reset(bool reset_reference=false) noexcept;
    bool process(SpectralPeakFrames,std::size_t frames,float main_bpm) noexcept;
    std::span<const float> values() const noexcept {return std::span(bpms_).first(count_);}
    float mean_bpm() const noexcept {return mean_bpm_;}
    FrameTempoPathStatus status() const noexcept {return status_;}
private:
    bool configure(std::size_t,FrameTempoPathConfig);
    bool select(SpectralPeakFrames,std::size_t,float) noexcept;
    float equal_bin(std::span<const InterpolatedPeak>,float) const noexcept;
    float harmonic_bin(std::span<const InterpolatedPeak>,float) const noexcept;
    float nearby_bin(std::span<const InterpolatedPeak>,float,float) const noexcept;
    bool harmonic_match(float,float) const noexcept;
    bool learn_mean(float) noexcept;
    bool postprocess() noexcept;
    std::size_t run_end(std::size_t) const noexcept;
    FrameTempoPathConfig config_;
    std::size_t count_=0,stable_frames_=0;
    float bpm_ratio_=0,mean_bpm_=0;
    FrameTempoPathStatus status_=FrameTempoPathStatus::Unprocessed;
    std::vector<float> bpms_;
};
}
