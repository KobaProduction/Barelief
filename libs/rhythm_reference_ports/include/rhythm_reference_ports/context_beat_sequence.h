// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/context_beat_tracker.h>
#include <cstdint>
#include <vector>
namespace rhythm_reference_ports {
struct ContextBeatSequenceConfig {
    ContextBeatTrackerConfig tracker{0}; // zero window: source next-power-of-two recipe
    std::uint32_t hop_samples=512;
    std::int32_t delay_samples=0;
    unsigned tatums_per_beat=4;
};
struct ContextBeatFrame {
    bool scheduled=false,silent=false;
    float source_tactus=0,period_samples=0,bpm=0,confidence=0;
    std::int64_t beat_sample=0;
};
// Aubio tempo.c's block/clock scheduling, with explicit reversed-weighted ODF,
// unbiased autocorrelation and silence inputs. History accumulation, weighting,
// reversal, correlation, FFT, whitening and peak-picking live outside this class.
// Boundary ordering, fractional-zero tactus, silence's
// last-beat update, and opposite beat/tatum delay signs retain source behavior.
// scheduled records a schedule match even when source_tactus equals zero.
// Sample counters are widened to avoid the original 32-bit wraparound.
class PreparedContextBeatSequence {
public:
    bool prepare(ContextBeatSequenceConfig={});
    static bool resolve_configuration(ContextBeatSequenceConfig,ContextBeatSequenceConfig&) noexcept;
    static bool storage_bytes(ContextBeatSequenceConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return tracker_.workspace_bytes()+offsets_.capacity()*sizeof(float);}
    void reset() noexcept;
    bool process(std::span<const float> reversed_weighted_history,std::span<const float> autocorrelation,
        bool silent,ContextBeatFrame&) noexcept;
    // Source query mutates tatum state only when explicitly polled:
    // 0=no tatum, 1=subdivision, 2=near last beat.
    unsigned poll_tatum() noexcept;
    bool set_tatum_signature(unsigned) noexcept;
    bool set_delay_samples(std::int32_t) noexcept;
    std::int32_t delay_samples() const noexcept {return config_.delay_samples;}
    unsigned tatum_signature() const noexcept {return config_.tatums_per_beat;}
    std::int64_t last_beat_sample() const noexcept;
    std::int64_t last_tatum_sample() const noexcept;
    float period_samples() const noexcept;
    const PreparedContextBeatTracker& tracker() const noexcept {return tracker_;}
    std::span<const float> scheduled_offsets() const noexcept {return std::span(offsets_).first(offset_count_);}
private:
    bool configure(ContextBeatSequenceConfig);
    bool valid_frame_inputs(std::span<const float>,std::span<const float>) const noexcept;
    bool advance_block(std::span<const float>,std::span<const float>) noexcept;
    ContextBeatFrame observe_frame(bool silent) noexcept;
    float scheduled_fraction(bool silent,bool& matched) noexcept;
    ContextBeatSequenceConfig config_;
    PreparedContextBeatTracker tracker_;
    std::vector<float> offsets_;
    std::size_t offset_count_=0;
    std::ptrdiff_t block_position_=0;
    std::uint64_t total_=0,last_beat_=0,last_tatum_=0;
};
}
