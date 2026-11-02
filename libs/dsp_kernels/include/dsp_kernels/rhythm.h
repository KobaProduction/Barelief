#pragma once
#include <dsp_kernels/onsets.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>

namespace dsp_kernels::rhythm {
// Native adaptation of METHOD-OWNER B-R4.4 (AI-Science 1373ee85).
// Analytical values/scores use Float32; time coordinates retain Double and
// clock/sample/sequence counters retain exact integer representations.
// Scores are heuristic decision scores, never calibrated probabilities.
enum class Status { Unknown, Ambiguous, Determined };
enum class Mode { Light, Full, Gated };
enum class Kind { Bass, Snare, Clap, Hat, Unknown };
enum class Reset { None, Epoch, Profile, Gap, Saturated };
enum class Schedule { Light, Full, Gated, History, Gap, Unambiguous, Budget };
enum class Reason {
    None, NoEvents, InsufficientEvidence, Stale, Octave, Weak,
    PrimaryConflict, SubdivisionConflict, NoAnchor, NoIndependentAccents,
    NoComparableOctave, AccentContrast, UnrelatedOctave, TypedPhase, Relocked
};
struct Config {
    unsigned feature_rate_fps=60, acf_window=128, acf_min_frames=90;
    unsigned feature_count=64, onset_channel_count=0;
    std::array<OnsetBand,max_onset_channels> onset_channels{};
    float min_bpm=60, max_bpm=180;
    double min_ioi_s=.15, max_ioi_s=2;
    float onset_threshold=.08f;
    double onset_refractory_s=.085;
    float onset_noise_multiplier=3;
    double onset_merge_s=.030;
    unsigned onset_min_positive_u8=8, min_events=3, max_provenance_groups=8;
    float score_threshold=.65f, score_margin=.14f, phase_threshold=.65f;
    double max_age_s=1.5, max_event_age_s=12;
    unsigned refinement_gap_frames=4, default_budget_ops=30000;
    float octave_accent_min=.78f, octave_accent_margin=.18f;
    float accent_phase_tolerance=.14f, accent_min_strength=.5f;
    float subdivision_min_strength=.25f;
    double accent_horizon_s=8;
    unsigned change_min_intervals=3;
    float change_stability_relative=.10f, change_separation_relative=.17f;
    double change_hold_s=2.5;
    float change_min_strong_strength=.65f;
    float phase_innovation_tolerance_fraction=.14f, relock_primary_period_tolerance=.025f;
    bool operator==(const Config&) const=default;
};
// IDs are prepared by the caller, stable within an epoch. Group 0 is the
// built-in spectral-onset source. Distinct IDs must mean independent observers.
// IDs also define deterministic ordering (assign them in lexical name order).
struct Accent {
    double t_obs=0;
    float strength=0;
    Kind kind=Kind::Unknown;
    std::uint32_t provenance=0, group=0;
};
using Features=std::variant<std::span<const std::uint8_t>,std::span<const float>>;
// Cumulative telemetry for accepted Float32 features. Values exactly one do not clip.
// Counts saturate at UINT64_MAX; the maximum includes accepted values below one.
struct ClippingStatistics {
    std::uint64_t clipped_frames=0,clipped_values=0;
    float maximum_before_clamp=0;
};
struct Frame {
    std::uint64_t epoch=0, sequence=0, sample_index=0, profile=0;
    double sample_rate_hz=0, t_obs=0, t_ready=0, t_pub=0;
    // UInt8 is explicitly /255. Finite Float32 amplitudes above one are clamped
    // to one for analysis and reported by clipping_statistics(); values in0..1
    // retain their exact Float32 value. Negative/nonfinite amplitudes are refused.
    // The borrowed view is consumed synchronously and is never modified.
    Features features;
    // nullopt invokes the spectral observer; an empty span disables it.
    std::optional<std::span<const Accent>> accents;
    std::optional<unsigned> budget_ops;
    bool saturated=false;
};
struct Estimate {
    Status status=Status::Unknown;
    Reason reason=Reason::None;
    std::optional<float> score;
    std::optional<double> t_obs, t_ready, age_s;
    double t_pub=0;
};
struct Hierarchy {
    bool available=false;
    float score=0, coverage=0, fit=0, weak_offbeat=0;
    std::optional<double> phase;
    std::uint8_t groups=0;
    unsigned strong_count=0, weak_count=0;
};
struct Candidate {
    float bpm=0;
    double period_s=0;
    float score=0,ioi=0,acf=0,phase_support=0,phase_penalty=0;
    std::optional<double> phase, t_obs, phase_t_obs;
    std::optional<double> tactus_phase;
    std::optional<float> tactus_support;
    std::optional<double> tactus_t_obs;
    std::optional<double> subdivision_phase;
    std::optional<float> subdivision_support;
    std::optional<double> subdivision_t_obs;
    std::optional<std::uint32_t> phase_group;
    std::array<float,8> group_support{};
    Hierarchy accent;
};
struct Event {
    Accent observation;
    std::uint64_t sequence=0;
    bool evidence_omitted=false;
    double t_ready=0;
    std::uint16_t source_channels=0; // Correlated spectral channels, not votes.
};
struct Change {
    bool present=false, acoustic_only=false;
    float from_bpm=0,to_bpm=0;
    double point_s=0,confirm_s=0;
    float spread=0;
    std::uint32_t group=0;
    unsigned intervals=0;
};
struct PackingStatistics {unsigned nonzero_bands=0,clipped_bands=0;};
// Float32 R4 diagnostic profile: raw RFFT4096 coefficients, DC discarded,
// Nyquist retained; disjoint linear-bin root-sum-square bands, clipped to0..1.
// Both projections require64/96 outputs. Failure preserves output/statistics.
bool project_rfft_features(std::span<const float> interleaved,std::span<float> output,
                           PackingStatistics& statistics) noexcept;
// Positive peak amplitudes k=1..2048, same band partition and0..1 clipping.
bool project_peak_amplitude_features(std::span<const float> amplitudes,std::span<float> output,
                                     PackingStatistics& statistics) noexcept;
// Explicit byte encoders for external formats; not the analytical Graph path.
bool pack_rfft(std::span<const float> interleaved,std::span<std::uint8_t> output,
               PackingStatistics& statistics) noexcept;
// Reuse the spectrogram frontend: normalized positive peak amplitudes k=1..2048.
// Packing retains amplitude scale; 64/96 bands use root-sum-square energy.
bool pack_peak_amplitudes(std::span<const float> amplitudes,std::span<std::uint8_t> output,
                          PackingStatistics& statistics) noexcept;
struct Octave {
    Status status=Status::Unknown; // Unknown means no relevant octave pair.
    Reason reason=Reason::None;
    std::optional<float> chosen_bpm,gap;
    std::uint8_t groups=0;
};
struct Output {
    Estimate accent_events, tempo, rhythm_grid, tempo_dynamics, evidence, diagnostics;
    std::optional<float> bpm;
    std::optional<double> period_s,grid_period_s,grid_phase_s,next_beat_s;
    std::optional<float> bpm_per_s,raw_support,periodicity;
    std::array<Candidate,42> candidates{};
    std::size_t candidate_count=0;
    std::array<Event,256> events{};
    std::size_t event_count=0;
    std::array<OnsetObservation,max_onset_channels> onset_channels{};
    unsigned onset_channel_count=0;
    unsigned onset_fusion_dropped=0;
    Octave octave;
    Change change, previous_episode;
    float activity=0,onset_contrast=0;
    unsigned history_frames=0, positive_changes=0, cost_units=0, possible_cost_units=0;
    unsigned evidence_group_overflow=0, subdivision_recovery_count=0;
    bool heavy_executed=false;
    Schedule schedule=Schedule::History;
    Reset reset=Reset::None;
    std::optional<double> last_acf_t_obs, tracking_start_s, confirmed_strong_ioi_s;
    std::optional<double> pending_primary_since, pending_subdivision_since;
    std::optional<std::uint32_t> tempo_anchor_group;
};
struct LagEvidence {float bpm=0,score=0;std::size_t lag=0;};
struct AnalysisView {
    std::span<const float> features,positive_differences;
    // Oldest-first history is the concatenation of these two ring segments.
    std::span<const float> flux_first,flux_second,centered_flux;
    // Contiguous lags beginning at first_lag. A rejected overlap is -1;
    // lag_evidence contains only accepted overlaps, including zero scores.
    std::span<const float> lag_scores;
    std::span<const LagEvidence> lag_evidence;
    std::size_t first_lag=0;
    std::optional<double> acf_t_obs;
    double t_obs=0;
    unsigned feature_rate_fps=0;
};
class Analyzer {
public:
    Analyzer();
    ~Analyzer();
    Analyzer(Analyzer&&) noexcept;
    Analyzer& operator=(Analyzer&&) noexcept;
    // Copies allocate on the cold preparation path only.
    Analyzer(const Analyzer&);
    Analyzer& operator=(const Analyzer&);
    // Cold path only. Rejects unsupported capacities; failure preserves state.
    bool prepare(const Config& config={}, Mode mode=Mode::Gated);
    // Clears processing history, retaining cumulative clipping telemetry.
    void reset() noexcept;
    // Compatible state copies include cumulative clipping telemetry.
    bool copy_state_from(const Analyzer& other) noexcept;
    // Invalid frames are rejected atomically. Accepted processing allocates no
    // memory. The result is owned until the next accepted frame/reset.
    bool process(const Frame& frame) noexcept;
    const Output& output() const noexcept;
    // Successful prepare starts at zero; source/gap/profile resets retain totals.
    // Invalid frames do not change these counters or their maximum.
    ClippingStatistics clipping_statistics() const noexcept;
    // Borrowed current state, valid until an accepted frame/reset/prepare.
    // Invalid input preserves it along with the baseline's atomic state.
    AnalysisView analysis() const noexcept;
    static std::size_t workspace_bytes() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
