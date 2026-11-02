#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dsp_kernels {
struct BeatTrackerConfig {
    float frame_rate=44100.f/512.f,tightness=100.f;
    std::size_t maximum_period_frames=2048;
    bool trim=true;
    bool operator==(const BeatTrackerConfig&) const=default;
};
// Dynamic-programming beat path, adapted from librosa 1.0.0 (ISC).
// Explicit oldest-first onset envelope and BPM (one value or one per frame).
// No spectrum, history, tempo estimation, or probability calibration is hidden.
// This is an offline/window estimate: future context can revise every beat.
// Output is a 0/1 mask on the input frame grid, NOT confirmed live beat events.
// Float32 arithmetic, prepared storage, no allocations during process().
// Rounded beat periods must be >=2 and <=maximum_period_frames; degenerate
// faster tempi in the original DP can produce self-links and are rejected.
class PreparedBeatTracker {
public:
    bool prepare(std::size_t maximum_frames,const BeatTrackerConfig& config={});
    bool process(std::span<const float> envelope,std::span<const float> bpm,std::span<std::uint8_t> beats) noexcept;
    bool silence(std::size_t frames,std::span<std::uint8_t> beats) noexcept;
    static bool storage_bytes(std::size_t maximum_frames,const BeatTrackerConfig&,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    // Borrowed diagnostics from the last successful process; invalid on failure
    // or prepare. Signed finite envelopes are accepted; no positive local score,
    // an empty input, or a single frame produces an empty beat mask.
    std::span<const float> local_scores() const noexcept {return {local_.data(),result_frames_};}
    std::span<const float> cumulative_scores() const noexcept {return {cumulative_.data(),result_frames_};}
    std::span<const std::int32_t> backlinks() const noexcept {return {backlink_.data(),result_frames_};}
private:
    void allocate(std::size_t frames);
    void clear(std::size_t frames) noexcept;
    bool estimate(std::span<const float> envelope) noexcept;
    bool input(std::span<const float>,std::span<const float>) noexcept;
    bool normalize(std::span<const float>) noexcept;
    void window(std::int32_t period) noexcept;
    bool local_scores(std::size_t frames) noexcept;
    bool dynamic_program(std::size_t frames) noexcept;
    void advance(std::size_t frame,float threshold,bool& first) noexcept;
    std::size_t tail(std::size_t frames) noexcept;
    bool trim(std::size_t frames) noexcept;
    float trim_threshold(std::size_t frames) noexcept;
    std::size_t maximum_frames_=0,result_frames_=0;
    BeatTrackerConfig config_;
    std::vector<float> normalized_,local_,cumulative_,scratch_,window_,log_intervals_;
    std::vector<std::int32_t> periods_,backlink_;
    std::vector<std::uint8_t> mask_;
};
}
