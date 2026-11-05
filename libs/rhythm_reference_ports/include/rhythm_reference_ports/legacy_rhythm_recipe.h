// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numerical_precision.h>
#include <rhythm_reference_ports/spectral_onset.h>
#include <rhythm_reference_ports/spectral_band_ratios.h>
#include <rhythm_reference_ports/band_tempo_features.h>
#include <rhythm_reference_ports/frame_silence_policy.h>
#include <rhythm_reference_ports/comb_period_phase.h>
#include <rhythm_reference_ports/period_phase_ticks.h>
#include <rhythm_reference_ports/lag_observation_pool.h>
#include <rhythm_reference_ports/tick_sequence_refinement.h>
#include <rhythm_reference_ports/lag_tempo_vote.h>
#include <rhythm_reference_ports/frame_stage_outputs.h>
#include <dsp_kernels/recursive_filter.h>
#include <dsp_kernels/tempogram.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct LegacyRhythmRecipeConfig {
    float sample_rate=44100,minimum_interval_seconds=.24f,last_beat_interval_seconds=.1f;
    std::size_t audio_frame=1024,audio_hop=256,period_window=1024,evaluation_hop=1024;
    int minimum_bpm=40,maximum_bpm=208;
    bool use_onset=true,use_bands=true;
    FrameSilenceConfig silence;
    dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32;
};
struct LegacyRhythmRecipeLimits {
    std::size_t maximum_pcm_samples=0,maximum_ticks=0,maximum_pool_values=0;
};
enum class LegacyRhythmStatus {NotProcessed,Ready,InvalidInput,FeatureFailure,PeriodFailure,
                               TickFailure,PoolCapacity,RefinementFailure,VoteFailure};
struct LegacyRhythmFrameStages {
    SpectralFrameStages spectrum;
    std::span<const float> conditioned_frame,features,band_power,band_edges_hz;
    // Source frequency bands retain these omitted-forwarding defaults.
    float band_sample_rate=44100;
    std::size_t band_frame_time_samples=512,evaluation_hop=0;
    // Current source cadence: periods/phases may repeat between evaluations.
    std::span<const float> periods,phases,ticks,matching_periods;
};
// Complete outdated Essentia RhythmExtractor numerical recipe. Three feature
// layouts (11/3/8) share centered source framing/noise and Hann raw Fourier.
// HFC -> [1,-1] -> positive clip, complex ODF, and FrequencyBands->TempoScaleBands
// are separate reusable operations. Source band Fs=44100 and frameTime=512 are
// intentionally NOT replaced with the outer Fs/hop (original omits forwarding).
// TempoTap evaluates at its source cadence; TempoTapTicks consumes EVERY frame.
// Pool repeats cumulative matching-period rows, represented by prefix metadata.
// Source's commented-out EOF feature injection stays disabled. Final extension,
// silence pruning, BPM vote and IBI stages reuse existing kernels.
// process() starts source reset + complete batch; reset retains generator and
// TempoTapTicks matching state as originals do. reset(true) explicitly clears
// matches. All output/pool bounds are caller-supplied; overflow is a failure.
// Source quality label: outdated; no claim of beat confirmation or probability.
// Frame-stage outputs follow features + source cadence tracking. Conditioned
// PCM is the frame after the original silence/noise policy, before Hann; its
// coordinate remains the outer centered PCM frame, not inner FFT frame0.
// Source: https://github.com/MTG/essentia/blob/b9fa6cb674ca43dfb94d28d293aeda441c6745db/src/algorithms/rhythm/rhythmextractor.cpp#L195-L228
class PreparedLegacyRhythmRecipe {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(LegacyRhythmRecipeLimits,LegacyRhythmRecipeConfig,std::span<const float> tempo_hints_seconds,std::size_t& bytes,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(LegacyRhythmRecipeLimits,LegacyRhythmRecipeConfig={},
                 std::span<const float> tempo_hints_seconds={},const dsp_kernels::FourierBackendFactory* =nullptr);
    void reset(bool clear_matches=false) noexcept;
    bool process(std::span<const float>,FrameStageOutputPort<LegacyRhythmFrameStages> output={}) noexcept;
    LegacyRhythmFrameStages frame_stages() const noexcept;
    std::span<const float> ticks() const noexcept;
    std::span<const float> intervals() const noexcept;
    std::span<const float> bpm_estimates() const noexcept;
    float bpm() const noexcept {return status_==LegacyRhythmStatus::Ready?vote_.bpm():0;}
    SilenceFrameBounds silence_bounds() const noexcept {return silence_.bounds();}
    const PreparedLagObservationPool& lag_pool() const noexcept {return pool_;}
    LegacyRhythmStatus status() const noexcept {return status_;}
private:
    void allocate_frame_buffers(std::size_t);
    bool process_frames(std::span<const float>,FrameStageOutputPort<LegacyRhythmFrameStages>) noexcept;
    bool configure(LegacyRhythmRecipeLimits,LegacyRhythmRecipeConfig,std::span<const float>,const dsp_kernels::FourierBackendFactory*);
    bool prepare_frames(const dsp_kernels::FourierBackendFactory*);
    bool prepare_features();
    bool prepare_tracking(std::span<const float>,const dsp_kernels::FourierBackendFactory*);
    bool prepare_results();
    bool load_frame(std::span<const float>,std::size_t) noexcept;
    bool features() noexcept;
    bool onset_features() noexcept;
    bool band_features() noexcept;
    bool track() noexcept;
    bool finish() noexcept;
    std::size_t feature_count() const noexcept;
    LegacyRhythmRecipeLimits limits_;
    LegacyRhythmRecipeConfig config_;
    LegacyRhythmStatus status_=LegacyRhythmStatus::NotProcessed;
    std::size_t maximum_frames_=0,candidate_count_=0,frame_index_=0;
    bool frame_ready_=false;
    dsp_kernels::PreparedTemporalWindow framing_;
    dsp_kernels::PreparedFourierTempogram spectrum_;
    dsp_kernels::PreparedRecursiveFilter derivative_;
    PreparedFrameSilencePolicy silence_;
    PreparedSpectralOnset hfc_,complex_onset_;
    PreparedFrequencyBandEnergy bands_;
    PreparedBandTempoFeatures scaled_bands_;
    PreparedFeaturePeriodTracker periods_;
    PreparedPeriodPhaseTicks ticks_;
    PreparedLagObservationPool pool_;
    PreparedTickSequenceRefinement refinement_;
    PreparedLagTempoVote vote_;
    std::vector<float> frame_,complex_,magnitude_,phase_,features_,band_power_,candidates_;
};
}
