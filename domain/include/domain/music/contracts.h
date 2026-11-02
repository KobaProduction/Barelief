#pragma once
#include <domain/graph/processing_model.h>

namespace domain {
struct ProcessingDiagnostic;
bool spectral_descriptor_kind(std::string_view) noexcept;
struct SpectralDescriptorParameters {
    bool half_wave=false,weighted=false,squared=false;
    float magnitude_threshold=0;
};
bool spectral_descriptor_parameters(const ResolvedProcessingNode&,SpectralDescriptorParameters&) noexcept;
signal_contracts::Status spectral_descriptor_output_contract(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
enum class OnsetStrengthAggregation {Mean,Median,Sum,Features};
struct OnsetStrengthParameters {
    std::size_t lag=1,max_size=1,n_fft=2048,hop_length=512;
    bool center=true,detrend=false;
    OnsetStrengthAggregation aggregation=OnsetStrengthAggregation::Mean;
};
bool onset_strength_parameters(const ResolvedProcessingNode&,OnsetStrengthParameters&) noexcept;
signal_contracts::Status onset_strength_output_contract(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
constexpr std::size_t rhythm_output_count=12;
constexpr std::size_t tempo_evidence_inputs=6,tempo_evidence_outputs=4,tempo_candidate_columns=7;
// Input ports: Fourier short/long, linear ACF short/long, filled short/long.
// Output 0: eight Float32 rows [BPM, score, Fshort, Flong, ACFshort, ACFlong,
// origin mask], zero padded; outputs 1/2/3: UInt32 count, Float32 maturity,
// UInt8 harmonic ambiguity. Scores are correlated support, not probability.
struct TempoEvidenceParameters {
    std::uint64_t frame_rate_fps=60;
    std::size_t short_fft=512,long_fft=4096;
    float min_bpm=40,max_bpm=240,min_observation_s=4,mature_observation_s=8;
};
bool tempo_evidence_parameters(const ConfiguredProcessingNode&,TempoEvidenceParameters&) noexcept;
bool tempo_evidence_parameters(const ResolvedProcessingNode&,TempoEvidenceParameters&) noexcept;
bool tempo_evidence_inputs_valid(const ConfiguredProcessingNode&,const TempoEvidenceParameters&) noexcept;
signal_contracts::Status tempo_evidence_output_contracts(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,std::array<OwnedProcessingContract,tempo_evidence_outputs>&) noexcept;
enum class RhythmFeatureRepresentation {Complex,PeakAmplitude};
bool rhythm_feature_representation(const ResolvedProcessingNode&,RhythmFeatureRepresentation&) noexcept;
signal_contracts::Status rhythm_features_output_contract(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
signal_contracts::Status rhythm_output_contracts(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,std::array<OwnedProcessingContract,rhythm_output_count>&) noexcept;
ProcessingDiagnostic frequency_normalizer_diagnostic(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&) noexcept;

signal_contracts::Status validate_frequency_normalizer(const ConfiguredProcessingNode&,const signal_contracts::NumericContract&) noexcept;
signal_contracts::Status frequency_profile_coefficient_coordinates(const ConfiguredProcessingNode&,
    signal_contracts::NumericContract&) noexcept;
// Canonical geometry of the PC music operations. Refusal preserves owner.
// Composite [low][mid][high][notes], full/selected peak agreement, and notes.
signal_contracts::Status musical_spectrogram_output_contracts(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,std::array<OwnedProcessingContract,4>&,
    std::array<std::size_t,4>* section_counts=nullptr) noexcept;
signal_contracts::Status music_output_contract(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&, OwnedProcessingContract&) noexcept;
ProcessingDiagnostic note_salience_calibration_diagnostic(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&) noexcept;
}
