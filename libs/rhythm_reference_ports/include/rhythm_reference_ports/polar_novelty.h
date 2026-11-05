// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
enum class PolarNovelty {Complex,ComplexPhase,BeatEmphasis};
// Exact source per-bin distances of Essentia OnsetDetection/Global.
// Complex: magnitude prediction, phase 2*p1-p2, Euclidean distance.
// ComplexPhase: current magnitude, phase 2*p1+p2, squared distance.
// BeatEmphasis: previous magnitude, phase 2*p1+p2, squared distance.
// The plus sign in the latter two is intentional original behavior.
// No FFT, ERB projection, aggregation or nonlinear scaling is hidden here.
class PreparedPolarNovelty {
public:
    bool prepare(std::size_t bins,PolarNovelty=PolarNovelty::Complex);
    static bool storage_bytes(std::size_t bins,PolarNovelty,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {
        return (magnitude_.capacity()+phase_.capacity()+older_.capacity()+staging_.capacity())*sizeof(float);
    }
    void reset() noexcept;
    bool process(std::span<const float> magnitude,std::span<const float> phase,
                 std::span<float> differences) noexcept;
private:
    float distance(std::size_t,float,float) const noexcept;
    PolarNovelty method_=PolarNovelty::Complex;
    std::vector<float> magnitude_,phase_,older_,staging_;
};
// Source spectral RMS = sqrt(sum(magnitude^2))/bins, not sqrt(mean).
// First-frame click suppression, then half-wave rectified temporal difference.
class PreparedSpectralRmsFlux {
public:
    void reset() noexcept {previous_=0;first_=true;}
    bool process(std::span<const float> magnitude,float& difference) noexcept;
private:
    float previous_=0;
    bool first_=true;
};
// Source amp2db used after Mel power projection: 2*lin2db, -200 below 1e-10.
bool amplitude_decibels(std::span<const float> input,std::span<float> output) noexcept;
}
