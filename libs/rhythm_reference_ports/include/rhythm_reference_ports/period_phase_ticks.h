// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct PeriodPhaseTickConfig {
    std::size_t evaluation_hop=512,audio_hop=256;
    float sample_rate=44100,maximum_candidate_frames=4096;
};
enum class PeriodPhaseTickStatus {Unprocessed,Ready,NoCandidates,InvalidInput,CapacityExceeded,NonAdvancingPhase};
// Source period/phase voting and tick generation. Call on every feature frame,
// including empty estimates, to preserve the source counter/time origin.
// Period zero is filtered; negative phase and negative initial tick times are
// retained. The 4x phase[4],phase[3],phase[2] boost uses ORIGINAL phase indices.
// matchingPeriods is source output/state: appended across nonempty calls and
// cleared by an empty candidate call. Reset preserves matches by default, as
// the original resets only the frame counter and nextPhase; reset(true) clears.
// Storage bounds are explicit; capacity exhaustion never silently drops data.
// Source Float32 non-advancing tick/modulo loops return a specific failure.
class PreparedPeriodPhaseTicks {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_candidates,std::size_t maximum_matches,std::size_t maximum_ticks,PeriodPhaseTickConfig,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_candidates,std::size_t maximum_matching_periods,
                 std::size_t maximum_ticks_per_call,PeriodPhaseTickConfig={});
    void reset(bool clear_matches=false) noexcept;
    bool process(std::span<const float> periods,std::span<const float> phases) noexcept;
    std::span<const float> ticks() const noexcept;
    std::span<const float> matching_periods() const noexcept {return std::span(matches_).first(match_count_);}
    std::int32_t next_phase() const noexcept {return next_phase_;}
    std::uint64_t consumed_frames() const noexcept {return frames_;}
    PeriodPhaseTickStatus status() const noexcept {return status_;}
private:
    bool configure(std::size_t,std::size_t,std::size_t,PeriodPhaseTickConfig);
    bool valid(std::span<const float>,std::span<const float>) const noexcept;
    std::size_t retain(std::span<const float>,std::span<const float>) noexcept;
    std::size_t boost(std::span<const float>,std::size_t) noexcept;
    bool closest(std::span<float>,float&) noexcept;
    bool period_vote(std::size_t,float&) noexcept;
    bool phase_vote(std::size_t,float&) noexcept;
    bool emit(float period,float phase) noexcept;
    bool update_next_phase(float period,float phase) noexcept;
    PeriodPhaseTickConfig config_;
    std::size_t maximum_candidates_=0,match_count_=0,tick_count_=0;
    std::uint64_t frames_=0;
    std::int32_t next_phase_=-1;
    float frame_time_=0,match_sum_=0;
    PeriodPhaseTickStatus status_=PeriodPhaseTickStatus::Unprocessed;
    std::vector<float> periods_,phases_,matches_,ticks_;
    std::vector<std::uint64_t> counts_;
};
}
