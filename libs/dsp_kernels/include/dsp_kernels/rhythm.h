#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace dsp_kernels::rhythm {
// Native adaptation of METHOD-OWNER B-R4.4 (AI-Science 1373ee85).
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
    double min_bpm=60, max_bpm=180, min_ioi_s=.15, max_ioi_s=2;
    double onset_threshold=.08, onset_refractory_s=.085;
    unsigned onset_min_positive_u8=8, min_events=3, max_provenance_groups=8;
    double score_threshold=.65, score_margin=.14, phase_threshold=.65;
    double max_age_s=1.5, max_event_age_s=12;
    unsigned refinement_gap_frames=4, default_budget_ops=30000;
    double octave_accent_min=.78, octave_accent_margin=.18;
    double accent_phase_tolerance=.14, accent_min_strength=.5;
    double subdivision_min_strength=.25, accent_horizon_s=8;
    unsigned change_min_intervals=3;
    double change_stability_relative=.10, change_separation_relative=.17;
    double change_hold_s=2.5, change_min_strong_strength=.65;
    double phase_innovation_tolerance_fraction=.14, relock_primary_period_tolerance=.025;
    bool operator==(const Config&) const=default;
};
// IDs are prepared by the caller, stable within an epoch. Group 0 is the
// built-in spectral-onset source. Distinct IDs must mean independent observers.
// IDs also define deterministic ordering (assign them in lexical name order).
struct Accent {
    double t_obs=0, strength=0;
    Kind kind=Kind::Unknown;
    std::uint32_t provenance=0, group=0;
};
struct Frame {
    std::uint64_t epoch=0, sequence=0, sample_index=0, profile=0;
    double sample_rate_hz=0, t_obs=0, t_ready=0, t_pub=0;
    std::span<const std::uint8_t> features;
    // nullopt invokes the spectral observer; an empty span disables it.
    std::optional<std::span<const Accent>> accents;
    std::optional<unsigned> budget_ops;
    bool saturated=false;
};
struct Estimate {
    Status status=Status::Unknown;
    Reason reason=Reason::None;
    std::optional<double> score, t_obs, t_ready, age_s;
    double t_pub=0;
};
struct Hierarchy {
    bool available=false;
    double score=0, coverage=0, fit=0, weak_offbeat=0;
    std::optional<double> phase;
    std::uint8_t groups=0;
    unsigned strong_count=0, weak_count=0;
};
struct Candidate {
    double bpm=0, period_s=0, score=0, ioi=0, acf=0, phase_support=0;
    double phase_penalty=0;
    std::optional<double> phase, t_obs, phase_t_obs;
    std::optional<double> tactus_phase, tactus_support, tactus_t_obs;
    std::optional<double> subdivision_phase, subdivision_support, subdivision_t_obs;
    std::optional<std::uint32_t> phase_group;
    std::array<double,8> group_support{};
    Hierarchy accent;
};
struct Event {
    Accent observation;
    std::uint64_t sequence=0;
    bool evidence_omitted=false;
};
struct Change {
    bool present=false, acoustic_only=false;
    double from_bpm=0, to_bpm=0, point_s=0, confirm_s=0, spread=0;
    std::uint32_t group=0;
    unsigned intervals=0;
};
struct PackingStatistics {unsigned nonzero_bands=0,clipped_bands=0;};
// R4 diagnostic profile: raw RFFT4096 coefficients, DC discarded, Nyquist
// retained, disjoint linear-bin energy bands. No Bark/Mel/instrument semantics.
bool pack_rfft(std::span<const float> interleaved,std::span<std::uint8_t> output,
               PackingStatistics& statistics) noexcept;
// Reuse the spectrogram frontend: normalized positive peak amplitudes k=1..2048.
// Packing retains amplitude scale; 64/96 bands use root-sum-square energy.
bool pack_peak_amplitudes(std::span<const float> amplitudes,std::span<std::uint8_t> output,
                          PackingStatistics& statistics) noexcept;
struct Octave {
    Status status=Status::Unknown; // Unknown means no relevant octave pair.
    Reason reason=Reason::None;
    std::optional<double> chosen_bpm, gap;
    std::uint8_t groups=0;
};
struct Output {
    Estimate accent_events, tempo, rhythm_grid, tempo_dynamics, evidence, diagnostics;
    std::optional<double> bpm, period_s, grid_period_s, grid_phase_s, next_beat_s;
    std::optional<double> bpm_per_s, raw_support, periodicity;
    std::array<Candidate,42> candidates{};
    std::size_t candidate_count=0;
    std::array<Event,256> events{};
    std::size_t event_count=0;
    Octave octave;
    Change change, previous_episode;
    double activity=0, onset_contrast=0;
    unsigned history_frames=0, positive_changes=0, cost_units=0, possible_cost_units=0;
    unsigned evidence_group_overflow=0, subdivision_recovery_count=0;
    bool heavy_executed=false;
    Schedule schedule=Schedule::History;
    Reset reset=Reset::None;
    std::optional<double> last_acf_t_obs, tracking_start_s, confirmed_strong_ioi_s;
    std::optional<double> pending_primary_since, pending_subdivision_since;
    std::optional<std::uint32_t> tempo_anchor_group;
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
    void reset() noexcept;
    bool copy_state_from(const Analyzer& other) noexcept;
    // Invalid frames are rejected atomically. Accepted processing allocates no
    // memory. The result is owned until the next accepted frame/reset.
    bool process(const Frame& frame) noexcept;
    const Output& output() const noexcept;
    static std::size_t workspace_bytes() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
