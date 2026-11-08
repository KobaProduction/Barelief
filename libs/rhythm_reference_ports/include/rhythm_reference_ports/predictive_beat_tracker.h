// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/scalar_history.h>
#include <array>
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
// Separate pure stages: no window accumulation, FFT, resampling or tempo state.
bool btrack_adaptive_threshold(std::span<const float>,std::span<float>) noexcept;
bool btrack_comb_observations(std::span<const float>,std::span<float>) noexcept;
class PreparedBTrackCombResponse {
public:
    bool prepare() noexcept;
    bool process(std::span<const float>,std::span<float>) const noexcept;
    void reset() noexcept {}
    std::size_t workspace_bytes() const noexcept {return 0;}
private:
    std::array<float,128> rayleigh_{};
};
// Tempo-state transition only. Observations arrive through an explicit input.
class PreparedCombTempoTransition {
public:
    bool prepare(std::size_t hop_samples=512) noexcept;
    static bool storage_bytes(std::size_t,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return 0;}
    void reset() noexcept;
    bool advance_observations(std::span<const float>,float&) noexcept;
    bool set_tempo(float) noexcept;
    bool fix_tempo(float) noexcept;
    void release_tempo() noexcept {fixed_=false;}
    float bpm() const noexcept {return bpm_;}
    std::span<const float> state_probabilities() const noexcept {return previous_;}
private:
    void prepare_weights() noexcept;
    void transition() noexcept;
    float select() noexcept;
    std::size_t tempo_index(float) const noexcept;
    std::size_t hop_=0;
    bool fixed_=false;
    float bpm_=120;
    std::array<float,41> observation_{},previous_{},fixed_previous_{},current_{};
    std::array<float,41*41> transitions_{};
};
struct BeatPredictionObservation {
    bool beat_due=false;
    std::ptrdiff_t next_beat=-1,next_prediction=10;
    float period_frames=0;
};
class PreparedBeatPrediction {
public:
    bool prepare(std::size_t history_size,float initial_period,float tightness=5,float alpha=.9f);
    static bool storage_bytes(std::size_t history_size,float initial_period,float tightness,float alpha,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return score_.workspace_bytes()+(future_.capacity()+weight_.capacity())*sizeof(float);}
    bool process(float onset,BeatPredictionObservation&) noexcept;
    bool set_period(float frames) noexcept;
    bool reset(float initial_period) noexcept;
    // Cold history resize: clears scores and changes period, retaining the
    // source prediction timers, alpha and tightness.
    bool resize_history(std::size_t history_size,float initial_period);
    bool indicate_tempo(float period) noexcept;
    std::span<const float> cumulative_scores() const noexcept {return score_.values();}
    float latest_cumulative_score() const noexcept {return score_.values().empty()?0:score_.values().back();}
private:
    std::size_t weighting(float period) noexcept;
    float cumulative(std::span<const float>,std::size_t start,std::size_t end,float onset,float alpha,
                     std::span<const float> future={}) const noexcept;
    bool predict() noexcept;
    void select_next_beat(std::size_t) noexcept;
    ScalarHistory score_;
    float period_=0,tightness_=5,alpha_=.9f;
    std::ptrdiff_t next_beat_=-1,next_prediction_=10;
    std::vector<float> future_,weight_;
};
}
