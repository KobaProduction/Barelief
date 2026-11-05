// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/interpolated_peaks.h>
#include <rhythm_reference_ports/scalar_history.h>
#include <dsp_kernels/spectral_autocorrelation.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct CombPeriodPhaseConfig {
    std::size_t number_frames=1024,audio_frame_samples=256;
    float sample_rate=44100;
    int minimum_bpm=40,maximum_bpm=208;
};
// Source TempoTap's per-feature numerical stages. No implicit history.
// Unbiased FFT ACF -> source four-harmonic weighted comb -> interpolated peak
// -> four-pulse phase response. Period and phase are in feature-frame units.
// Source detector coordinates stay tied to floor(N/4)-1 even when the phase
// array uses round(N/4). Invalid lag clears period to 0, phase to -1.
class PreparedCombPeriodPhase {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(CombPeriodPhaseConfig,std::span<const float> tempo_hints_seconds,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(CombPeriodPhaseConfig={},std::span<const float> tempo_hints_seconds={},const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process_feature(std::span<const float>,float& period,float& phase) noexcept;
    std::size_t frames() const noexcept {return config_.number_frames;}
    std::span<const float> weights() const noexcept {return weights_;}
    std::span<const float> correlation() const noexcept {return acf_;}
    std::span<const float> comb() const noexcept {return comb_;}
    std::span<const float> phase_response() const noexcept {return phase_response_;}
private:
    bool configure(CombPeriodPhaseConfig,std::span<const float>,const dsp_kernels::FourierBackendFactory*);
    bool geometry(CombPeriodPhaseConfig) noexcept;
    bool prepare_weights(std::span<const float>) noexcept;
    bool prepare_detector();
    bool estimate_period(std::span<const float>,float&) noexcept;
    bool estimate_phase(std::span<const float>,float&,float&) noexcept;
    CombPeriodPhaseConfig config_;
    float frame_time_=0,minimum_lag_=0,maximum_lag_=0;
    dsp_kernels::PreparedSpectralAutocorrelation autocorrelation_;
    PreparedInterpolatedPeaks detector_;
    std::vector<float> weights_,acf_,comb_,phase_response_;
};
// Source sequential buffer/evaluation cadence, using the existing mirrored
// ScalarHistory for each explicit feature. Reset zeros full history and phase
// of the evaluation cadence. No history clone/transpose on the processing path.
// The original reset's hardcoded 11-feature row is generalized to the explicit
// feature count; default 11 retains its normal source recipe.
class PreparedFeaturePeriodTracker {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t features,std::size_t evaluation_hop,CombPeriodPhaseConfig,std::span<const float> tempo_hints_seconds,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t features=11,std::size_t evaluation_hop=1024,
                 CombPeriodPhaseConfig={},std::span<const float> tempo_hints_seconds={},const dsp_kernels::FourierBackendFactory* =nullptr);
    void reset() noexcept;
    bool push(std::span<const float> features) noexcept;
    bool ready() const noexcept {return ready_;}
    std::span<const float> periods() const noexcept;
    std::span<const float> phases() const noexcept;
private:
    bool evaluate() noexcept;
    bool configure(std::size_t,std::size_t,CombPeriodPhaseConfig,std::span<const float>,const dsp_kernels::FourierBackendFactory*);
    PreparedCombPeriodPhase estimator_;
    std::size_t evaluation_hop_=0,pending_=0;
    bool ready_=false;
    std::vector<ScalarHistory> history_;
    std::vector<float> periods_,phases_;
};
}
