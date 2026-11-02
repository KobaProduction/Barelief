#pragma once
#include <cstddef>
#include <span>

namespace dsp_kernels {
enum class CorrelationNormalization {Raw,Unbiased,Biased,OverlapEnergy};
struct OverlapEnergyPolicy {
    float energy_floor=0,rejected_value=0,minimum=-1,maximum=1;
    bool multiply_energies=false;
};
// Explicit overlap-energy policy for an arbitrary contiguous lag interval.
// Each overlap at/below energy_floor returns rejected_value; accepted values
// are clamped to [minimum,maximum]. multiply_energies selects sqrt(Ea*Eb),
// otherwise sqrt(Ea)*sqrt(Eb). Existing correlation defaults are unchanged.
// Invalid geometry/policy/nonfinite input preserves output; numeric overflow
// may leave a partial row. Input/output must be disjoint; no allocation.
bool overlap_energy_autocorrelation(std::span<const float> input,std::size_t first_lag,
                                    std::span<float> output,OverlapEnergyPolicy={}) noexcept;
// Equal-length cross-correlation c(lag)=sum left[i]*right[i-lag], with an
// explicit signed first lag and contiguous output. The normalization uses
// overlap length, full input length, or overlap energy as above. No allocation;
// input and output spans must be disjoint, all requested lags overlap.
bool linear_crosscorrelation(std::span<const float> left,std::span<const float> right,
                             std::ptrdiff_t first_lag,std::span<float> output,
                             CorrelationNormalization normalization=CorrelationNormalization::Raw) noexcept;
// Explicit Float32 variant. Raw is a linear lag sum, Unbiased divides by
// overlap length, Biased by the input length. No centering is implicit.
bool linear_autocorrelation(std::span<const float> input,std::span<float> output,
                            CorrelationNormalization normalization) noexcept;
// Linear, overlap-energy-normalized autocorrelation at lags [0, output.size()).
// Centering/windowing are explicit caller operations. Signed values are [-1,1];
// a zero-energy overlap yields zero. Finite input, output.size() <= input.size(),
// disjoint spans required. Invalid input leaves output unchanged. No allocation.
bool linear_autocorrelation(std::span<const float> input,std::span<float> output) noexcept;
}
