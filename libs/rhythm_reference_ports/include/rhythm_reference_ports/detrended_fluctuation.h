// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
enum class FluctuationStatus {NotProcessed,Ok,TooShort,ZeroFluctuation,NonFiniteFluctuation,NonFiniteExponent};
struct FluctuationScalingResult {
    float score=0;
    std::size_t available_scales=0;
    FluctuationStatus status=FluctuationStatus::NotProcessed;
};
// Source Danceability DFA stage. Input is an explicitly integrated activity
// sequence. Source k < N-tau, jump=max(tau/50,1), fractional block divisor
// (N-tau)/jump and +3 corrected log slopes are retained. This is not a beat
// tracker or a confidence probability. Missing scales remain zero.
// A nonfinite numerical outcome is diagnosed and returns false; no silent
// clamp of a negative least-squares residual or duplicate-scale exponent.
class PreparedDetrendedFluctuation {
public:
    bool prepare(std::span<const std::size_t> scales);
    static bool required_workspace_bytes(std::size_t scale_count,std::size_t& bytes) noexcept {
        constexpr auto per_scale=sizeof(std::size_t)+2*sizeof(float);
        if(!scale_count || scale_count>SIZE_MAX/per_scale)return false;
        bytes=scale_count*per_scale-sizeof(float);return true;
    }
    bool process(std::span<const float> integrated) noexcept;
    const FluctuationScalingResult& result() const noexcept {return result_;}
    std::span<const std::size_t> scales() const noexcept {return scales_;}
    std::span<const float> fluctuations() const noexcept {return fluctuations_;}
    std::span<const float> exponents() const noexcept {return exponents_;}
    std::size_t workspace_bytes() const noexcept {
        return scales_.capacity()*sizeof(std::size_t)+(fluctuations_.capacity()+exponents_.capacity())*sizeof(float);
    }
private:
    bool publish_score(float) noexcept;
    bool calculate_scales(std::span<const float>) noexcept;
    bool scale(std::span<const float>,std::size_t index) noexcept;
    bool score() noexcept;
    std::vector<std::size_t> scales_;
    std::vector<float> fluctuations_,exponents_;
    FluctuationScalingResult result_;
};
}
