#pragma once
#include <cstdint>
#include <span>
namespace dsp_kernels {
// Uniform, data-range bins with explicit Float32 centre coordinates. Cutoff
// ties belong to the lower bin. A constant population falls in bin zero.
// Nonempty finite input, >=2 bins, equally sized count/centre outputs.
// Input/centres must be disjoint. No allocation, sorting or hidden history.
// Counts nearest nonnegative integer bins after division by an explicit step.
// Negative values map to bin zero; half-bin ties round upward. Counts are exact
// integers, with no data-range renormalization. Invalid input leaves counts intact.
bool rounded_bin_counts(std::span<const float> input,std::span<std::uint64_t> counts,
                        float step=1) noexcept;
bool uniform_value_histogram(std::span<const float> input,std::span<std::uint64_t> counts,
                             std::span<float> centers) noexcept;
}
