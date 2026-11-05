// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <span>
#include <cstddef>

namespace rhythm_reference_ports {
// Sum acf[a*(p+1)+b-1]*weight[p]/(2*a-1), a=1..harmonics,
// b=1-a..a-1, for p in [first,last). Other response bins are zero.
// Input and output spans must be disjoint; all lag accesses must be valid.
// Per-period accumulation order is harmonic then offset in both source recipes.
bool harmonic_comb_geometry(std::size_t input_samples,std::size_t response_samples,
                            std::size_t first,std::size_t stop,std::size_t harmonics) noexcept;
bool harmonic_comb_response(std::span<const float> acf,std::span<const float> weights,
                            std::size_t first,std::size_t last,std::size_t harmonics,
                            std::span<float> response) noexcept;
// Sum input[round(a*period+i)], a=0..pulses-1, for each output position i.
// Out-of-range indices are explicitly zero-extended instead of source assert/OOB.
bool periodic_phase_response(std::span<const float> input,float period,std::size_t pulses,
                             std::span<float> response) noexcept;
}
