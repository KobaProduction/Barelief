// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct HarmonicTempoDistance {float error_percent=0,ratio=0;};
bool harmonic_tempo_distance(float reference,float candidate,HarmonicTempoDistance&) noexcept;
bool approximately_equal_tempo(float left,float right,float tolerance_percent) noexcept;
bool harmonically_related_tempo(float left,float right,float tolerance_percent) noexcept;
bool greatest_common_tempo(float left,float right,float tolerance_percent,float& result) noexcept;
struct HarmonicTempoConfig {float reference_bpm=60,minimum_common_bpm=20,tolerance_percent=5;};
// Source HarmonicBpm: approximate GCD threshold, sorted tolerance clusters,
// then source harmonic-distance error and adjacent-cluster de-duplication.
// Config and input BPMs are >=1; low-level distance helpers accept any positive
// finite BPM. Unsafe source integer conversion is rejected only if GCD needs it.
class PreparedHarmonicTempoCandidates {
public:
    bool prepare(std::size_t maximum_candidates,HarmonicTempoConfig={});
    static bool required_workspace_bytes(std::size_t maximum,HarmonicTempoConfig,std::size_t& bytes) noexcept;
    bool process(std::span<const float> candidates) noexcept;
    std::span<const float> values() const noexcept {return std::span(result_).first(count_);}
    std::size_t workspace_bytes() const noexcept {return (candidates_.capacity()+result_.capacity())*sizeof(float);}
private:
    bool validate(std::span<const float>) const noexcept;
    bool retain(std::span<const float>,std::size_t&) noexcept;
    float cluster(std::size_t&,std::size_t) const noexcept;
    float error(float) const noexcept;
    void append(float best,float previous) noexcept;
    HarmonicTempoConfig config_;
    std::size_t count_=0;
    std::vector<float> candidates_,result_;
};
}
