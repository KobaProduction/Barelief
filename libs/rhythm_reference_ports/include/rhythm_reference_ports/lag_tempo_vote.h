// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <cstdint>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct LagTempoVoteConfig {
    float sample_rate=44100,maximum_bpm=1000;
    std::size_t audio_hop=256;
};
// Source legacy RhythmExtractor final BPM vote: nonzero lag -> BPM,
// divide-by-two/rounded-bin mode (first maximum), restore BPM, strict +/-5
// selection and arithmetic mean. Empty observations produce BPM0.
// Input contains the original flattened matchingPeriods rows, including any
// source repeats. maximum_bpm is an explicit workspace bound, not a clamp.
class PreparedLagTempoVote {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_lags,LagTempoVoteConfig,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_lags,LagTempoVoteConfig={});
    bool process(std::span<const float> matching_periods) noexcept;
    bool process(dsp_kernels::FloatSequenceSource,std::size_t count) noexcept;
    float bpm() const noexcept {return bpm_;}
    std::span<const float> estimates() const noexcept {return std::span(estimates_).first(count_);}
private:
    bool convert(dsp_kernels::FloatSequenceSource,std::size_t,std::size_t&) noexcept;
    bool select(std::size_t) noexcept;
    LagTempoVoteConfig config_;
    float bpm_=0;
    std::size_t count_=0;
    std::vector<float> bpms_,estimates_;
    std::vector<std::uint64_t> counts_;
};
}
