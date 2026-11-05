// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rhythm_reference_ports {
struct TempoOctaveClassifier {
    // The source second minimum 1.68126e-83 underflows to zero in Float32.
    std::array<float,3> minimum{.0321812f,0.f,50.1745f};
    std::array<float,3> maximum{.863237f,.449184f,208.807f};
    std::array<std::array<float,4>,3> weights{{
        {-1.955100f,.434800f,-4.644200f,3.289600f},
        {-3.040800f,2.759100f,-6.536700f,3.081000f},
        {-3.462400f,3.439700f,-9.489700f,1.629700f}}};
};
struct LagTempoConsensusResult {
    float bpm=0,uncorrected_bpm=0,octave_multiplier=1;
    std::size_t lag=0;
    std::array<float,3> features{};
};
// Source Gaussian lag histogram and three fixed linear SVM octave decisions.
// Learned scalar SVM coefficients are explicit classical model parameters,
// not neural inference. No detections gives BPM0; lag0 cannot define finite BPM.
// Gaussian support is explicitly zero outside the source's prepared table.
class PreparedLagTempoConsensus {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(float,TempoOctaveClassifier,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(float onset_frame_rate,TempoOctaveClassifier={}) noexcept;
    void reset() noexcept;
    bool add(std::int32_t lag) noexcept;
    bool finish(LagTempoConsensusResult&) const noexcept;
    std::span<const float> histogram() const noexcept {return histogram_;}
    std::uint64_t observations() const noexcept {return observations_;}
private:
    bool features(LagTempoConsensusResult&) const noexcept;
    float range_energy(float low,float high,float scale) const noexcept;
    float octave_multiplier(std::array<float,3>) const noexcept;
    float gaussian(std::int64_t) const noexcept;
    float frame_rate_=0;
    std::uint64_t observations_=0;
    TempoOctaveClassifier classifier_;
    std::array<float,2000> gaussian_{};
    std::array<float,414> histogram_{};
};
}
