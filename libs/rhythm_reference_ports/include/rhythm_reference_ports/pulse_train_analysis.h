// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
// Source EnhanceHarmonics: y[i] += y[2*i] + y[4*i] for i < N/4.
// Input and output may be identical; partially overlapping buffers are rejected.
bool enhance_correlation_harmonics(std::span<const float>,std::span<float>) noexcept;
// Independently observable pulse-train evaluation. Candidate positions are
// rounded to integer lags; four pulses at period, 2*period and 3*period/2
// contribute weights 1, .5 and .5. Phase maximum and population variance are
// normalized separately before their sum selects the first maximum.
// Fixes source reserve/index UB by preparing sized score buffers. A finite
// OSS window is explicitly zero-extended, fixing source out-of-range pulse
// reads. This repair must also be applied to the numerical comparison oracle.
// IEEE NaN normalized scores for zero sums are retained as source diagnostics;
// their first-max selection remains deterministic. No peaks yields lag=-1.
class PreparedPulseTrainScoring {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t maximum_lag,std::size_t maximum_candidates,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_lag,std::size_t maximum_candidates);
    bool process(std::span<const float> onset,std::span<const float> candidate_lags,float& selected_lag) noexcept;
    std::span<const float> magnitudes() const noexcept {return std::span(magnitudes_).first(count_);}
    std::span<const float> variances() const noexcept {return std::span(variances_).first(count_);}
    std::span<const float> scores() const noexcept {return std::span(scores_).first(count_);}
private:
    bool valid(std::span<const float>,std::span<const float>) const noexcept;
    float pulse(std::span<const float>,std::size_t phase,std::size_t lag) const noexcept;
    bool evaluate(std::span<const float>,std::size_t lag,std::size_t candidate) noexcept;
    std::size_t select() noexcept;
    std::size_t count_=0;
    std::vector<float> phases_,magnitudes_,variances_,scores_;
};
}
