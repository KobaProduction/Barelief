// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <dsp_kernels/spectral_autocorrelation.h>
#include <rhythm_reference_ports/scalar_history.h>
#include <rhythm_reference_ports/detail/predictive_reference_comb.h>
#include <dsp_kernels/numerical_precision.h>
#include <array>
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
// The source algorithm resamples a whole ODF window with libsamplerate
// SRC_SINC_BEST_QUALITY. The backend is explicit, including at ratio1; do not
// silently substitute identity/linear interpolation/Speex and call it parity.
struct OnsetWindowResamplingPort {
    void* context=nullptr;
    bool (*process)(void*,std::span<const float>,std::span<float>) noexcept=nullptr;
};
class PreparedCombTempoTransition {
public:
    bool prepare(std::size_t hop_samples=512,dsp_kernels::NumericalPrecision=dsp_kernels::NumericalPrecision::PortableFloat32,
                 const dsp_kernels::FourierBackendFactory* =nullptr);
    static bool storage_bytes(std::size_t hop_samples,std::size_t&,dsp_kernels::NumericalPrecision=dsp_kernels::NumericalPrecision::PortableFloat32,
                              const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept {return correlation_.workspace_bytes()+reference_.workspace_bytes();}
    void reset() noexcept;
    bool set_hop_samples(std::size_t) noexcept;
    bool process(std::span<const float> resampled_512,float& period_frames) noexcept;
    // Split portable graph boundary: analysis has no tempo-state transition.
    // advance_observations is called only after the predictor marks a beat.
    // These boundaries intentionally reject DesktopReference (raw oracle).
    bool analyze_observations(std::span<const float> resampled_512) noexcept;
    bool advance_observations(std::span<const float> observations_41,float& period_frames) noexcept;
    std::span<const float> balanced_autocorrelation() const noexcept {return acf_;}
    bool set_tempo(float bpm) noexcept;
    bool fix_tempo(float bpm) noexcept;
    void release_tempo() noexcept;
    float bpm() const noexcept {return bpm_;}
    std::span<const float> response() const noexcept {return comb_;}
    std::span<const float> tempo_observations() const noexcept {return observation_;}
    std::span<const float> state_probabilities() const noexcept {return previous_;}
    const detail::PreparedPredictiveReferenceComb& reference_stage() const noexcept {return reference_;}
private:
    bool configure(std::size_t,dsp_kernels::NumericalPrecision,const dsp_kernels::FourierBackendFactory*);
    void prepare_weights() noexcept;
    bool balanced_acf() noexcept;
    bool process_portable(std::span<const float>,float&) noexcept;
    void threshold(std::span<float>) noexcept;
    void comb_filter() noexcept;
    void observations() noexcept;
    void transition() noexcept;
    float select() noexcept;
    void publish_reference() noexcept;
    std::size_t tempo_index(float bpm) const noexcept;
    std::size_t hop_=0;
    bool fixed_=false;
    float bpm_=120;
    dsp_kernels::PreparedSpectralAutocorrelation correlation_;
    detail::PreparedPredictiveReferenceComb reference_;
    std::array<float,512> onset_{},acf_{},threshold_{};
    std::array<float,128> rayleigh_{},comb_{};
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
class PreparedPredictiveBeatTracker {
public:
    // Source PCM rate is44100Hz; hop is the source audio hop. The ODF frontend
    // remains an explicit separate block. The PCM recipe binds its prepared
    // whole-window resampler through this port.
    bool prepare(std::size_t hop_samples,OnsetWindowResamplingPort,
                 dsp_kernels::NumericalPrecision=dsp_kernels::NumericalPrecision::PortableFloat32,
                 const dsp_kernels::FourierBackendFactory* =nullptr);
    static bool storage_bytes(std::size_t hop_samples,std::size_t&,
                              dsp_kernels::NumericalPrecision=dsp_kernels::NumericalPrecision::PortableFloat32,
                              const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept {return tempo_.workspace_bytes()+prediction_.workspace_bytes()+onset_.workspace_bytes();}
    bool set_resampling_port(OnsetWindowResamplingPort port) noexcept {
        if(!port.process)return false;
        resampler_=port;return true;
    }
    bool reset() noexcept;
    // Source setHopSize computation; cold, may allocate. Keeps tempo priors,
    // fix state, estimated BPM and timers while resetting ODF/score histories.
    bool set_hop_samples(std::size_t);
    std::size_t hop_samples() const noexcept {return hop_;}
    bool process(float onset,BeatPredictionObservation&) noexcept;
    // Explicit source frontend boundary: abs(Double ODF)+Double epsilon is
    // rounded once into the Float32 history. Prediction/comb remain Float32.
    bool process_reference(double onset,BeatPredictionObservation&) noexcept;
    bool set_tempo(float bpm) noexcept;
    bool fix_tempo(float bpm) noexcept {return tempo_.fix_tempo(bpm);}
    void release_tempo() noexcept {tempo_.release_tempo();}
    float bpm() const noexcept {return tempo_.bpm();}
    std::span<const float> onset_window() const noexcept {return onset_.values();}
    std::span<const float> cumulative_scores() const noexcept {return prediction_.cumulative_scores();}
    float latest_cumulative_score() const noexcept {return prediction_.latest_cumulative_score();}
    const PreparedCombTempoTransition& tempo_stage() const noexcept {return tempo_;}
    std::span<const float> resampled_onsets() const noexcept {return resampled_;}
private:
    static void seed_onsets(ScalarHistory&,float period) noexcept;
    bool process_prepared(float,BeatPredictionObservation&) noexcept;
    PreparedCombTempoTransition tempo_;
    PreparedBeatPrediction prediction_;
    ScalarHistory onset_;
    OnsetWindowResamplingPort resampler_;
    std::array<float,512> resampled_{};
    std::size_t hop_=0;
};
}
