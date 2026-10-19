#pragma once
#include <domain/graph/processing_model.h>

namespace domain {
// Canonical output derivation shared by runtime admission and editor adapters.
// Transpose materializes an ordinary matrix and swaps both coordinates and axes.
signal_contracts::Status matrix_transpose_output_contract(const signal_contracts::NumericContract&,
    OwnedProcessingContract&) noexcept;
signal_contracts::Status matrix_window_output_contract(const signal_contracts::NumericContract&,
    OwnedProcessingContract&) noexcept;
signal_contracts::Status matrix_center_output_contract(const signal_contracts::NumericContract&,
    OwnedProcessingContract&) noexcept;
// Rows are independent signals; columns are chronological samples. Explicit
// Time/Seconds uses the graph's declared analysis cadence during execution.
signal_contracts::Status matrix_fft_output_contract(const signal_contracts::NumericContract&,
    std::size_t fft_size, OwnedProcessingContract&) noexcept;
signal_contracts::Status matrix_row_output_contract(const signal_contracts::NumericContract&,
    OwnedProcessingContract&) noexcept;

signal_contracts::Status array_generator_output_contract(const ConfiguredProcessingNode&,const signal_contracts::NumericContract&,OwnedProcessingContract&) noexcept;
// Empty typed coefficients select scalar fill. An empty destination validates only.
signal_contracts::Status array_generator_values(const ConfiguredProcessingNode&,std::span<float>,std::size_t& count) noexcept;
signal_contracts::Status array_concat_output_contract(const ConfiguredProcessingNode&,OwnedProcessingContract&) noexcept;
signal_contracts::Status array_product_output_contract(const ConfiguredProcessingNode&,OwnedProcessingContract&,std::size_t* frame_input=nullptr) noexcept;
}
