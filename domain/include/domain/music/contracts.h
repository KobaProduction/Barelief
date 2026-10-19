#pragma once
#include <domain/graph/processing_model.h>

namespace domain {
struct ProcessingDiagnostic;
ProcessingDiagnostic frequency_normalizer_diagnostic(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&) noexcept;

signal_contracts::Status validate_frequency_normalizer(const ConfiguredProcessingNode&,const signal_contracts::NumericContract&) noexcept;
// Canonical geometry of the PC music operations. Refusal preserves owner.
// Composite [low][mid][high][notes], full/selected peak agreement, and notes.
signal_contracts::Status musical_spectrogram_output_contracts(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,std::array<OwnedProcessingContract,4>&,
    std::array<std::size_t,4>* section_counts=nullptr) noexcept;
signal_contracts::Status music_output_contract(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&, OwnedProcessingContract&) noexcept;
}
