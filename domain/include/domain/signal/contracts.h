#pragma once
#include <domain/graph/processing_model.h>

namespace domain {
struct PowerToDbParameters {
    double reference=1,amin=1e-10,top_db=80;
    bool top_db_enabled=true;
    std::string_view reference_mode="value",axes="all";
};
bool power_to_db_parameters(const ResolvedProcessingNode&,PowerToDbParameters&) noexcept;
signal_contracts::Status power_to_db_output_contract(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;

struct MelFilterParameters {
    double sample_rate=48000,bin_hz=1,fmin_hz=0,fmax_hz=0,norm_p=2;
    std::size_t bands=50;
    std::uint64_t first_bin=1;
    std::span<const float> frequencies_hz;
    std::string_view scale="htk",norm="none";
};
bool mel_parameters(const ResolvedProcessingNode&,MelFilterParameters&) noexcept;
signal_contracts::Status mel_output_contract(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
bool declared_frequency_axis_matches(const signal_contracts::AxisCoordinates&,double step,std::uint64_t first);
// Scalar peak frequency and magnitude from explicit bin_hz/first_bin settings.
signal_contracts::Status spectrum_peak_output_contracts(const signal_contracts::NumericContract&,
    std::array<OwnedProcessingContract,2>&) noexcept;

// Full C2C planes contain N bins each. Real FFT separates DC from Re[1..N/2]
// and Im[1..N/2-1]; imaginary DC/Nyquist are identically zero.
// Neither the numerical transform nor its bin coordinates depend on Fs.
signal_contracts::Status fourier_output_contracts(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&, std::vector<OwnedProcessingContract>&) noexcept;
signal_contracts::Status complex_polar_output_contracts(const signal_contracts::NumericContract&,
    std::array<OwnedProcessingContract,2>&) noexcept;
signal_contracts::Status complex_polar_output_contracts(const ConfiguredProcessingNode&,
    const signal_contracts::NumericContract&,std::array<OwnedProcessingContract,2>&) noexcept;
signal_contracts::Status spectrum_amplitude_output_contract(const ConfiguredProcessingNode&,const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
signal_contracts::Status frequency_step_output_contract(const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
signal_contracts::Status explicit_frequency_grid_contract(const ConfiguredProcessingNode&,const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
signal_contracts::Status numeric_metadata_output_contract(const signal_contracts::NumericContract&,bool sample_rate,
    OwnedProcessingContract&) noexcept;
bool has_window_gain_output(std::string_view) noexcept;
double processing_window_gain(const ConfiguredProcessingNode&,std::size_t count,bool power=false) noexcept;
signal_contracts::Status window_gain_output_contract(const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
signal_contracts::Status window_function_coefficients(const ConfiguredProcessingNode&,std::span<float>) noexcept;
signal_contracts::Status window_function_coefficients(const ConfiguredProcessingNode&,std::span<double>) noexcept;
signal_contracts::Status amplitude_phase_output_contracts(const ConfiguredProcessingNode&,const signal_contracts::NumericContract&,std::array<OwnedProcessingContract,2>&) noexcept;
}
