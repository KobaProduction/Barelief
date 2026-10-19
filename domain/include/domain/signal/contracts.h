#pragma once
#include <domain/graph/processing_model.h>

namespace domain {
bool declared_frequency_axis_matches(const signal_contracts::AxisCoordinates&,double step,std::uint64_t first);
// Ordinary numeric outputs: Re/Im for bins 1..N-1 (complex) or 1..N/2
// (real), plus separate raw DC Re/Im scalars. No Fs/frequency coordinates.
// Scalar peak frequency and magnitude from explicit bin_hz/first_bin settings.
signal_contracts::Status spectrum_peak_output_contracts(const signal_contracts::NumericContract&,
    std::array<OwnedProcessingContract,2>&) noexcept;

signal_contracts::Status fourier_output_contracts(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&, std::array<OwnedProcessingContract,4>&) noexcept;
signal_contracts::Status complex_polar_output_contracts(const signal_contracts::NumericContract&,
    std::array<OwnedProcessingContract,2>&) noexcept;
signal_contracts::Status spectrum_amplitude_output_contract(const ConfiguredProcessingNode&,const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
signal_contracts::Status frequency_step_output_contract(const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
signal_contracts::Status explicit_frequency_grid_contract(const ConfiguredProcessingNode&,const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
signal_contracts::Status numeric_metadata_output_contract(const signal_contracts::NumericContract&,bool sample_rate,
    OwnedProcessingContract&) noexcept;
bool has_window_gain_output(std::string_view) noexcept;
double processing_window_gain(const ConfiguredProcessingNode&,std::size_t count,bool power=false) noexcept;
signal_contracts::Status window_gain_output_contract(const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
signal_contracts::Status amplitude_phase_output_contracts(const ConfiguredProcessingNode&,const signal_contracts::NumericContract&,std::array<OwnedProcessingContract,2>&) noexcept;
}
