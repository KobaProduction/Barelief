#pragma once
#include <cmath>
#include <span>
namespace dsp_kernels {
enum class FilterProjectionAccumulation {Separate,SequentialFma};
struct FilterProjectionOptions {
    bool compensated=true,square_input=false;
    FilterProjectionAccumulation accumulation=FilterProjectionAccumulation::Separate;
};
constexpr bool valid_filter_projection_options(FilterProjectionOptions options) noexcept {
    return (options.accumulation==FilterProjectionAccumulation::Separate || options.accumulation==FilterProjectionAccumulation::SequentialFma) &&
        (options.accumulation!=FilterProjectionAccumulation::SequentialFma || !options.compensated);
}
// Shared Float32 dot product for explicit filter coefficients.
bool project_filter_weights(std::span<const float> input,std::span<const float> weights,float& output,
                            FilterProjectionOptions options={}) noexcept;
struct FilterBandSupport {
    std::size_t first=0,count=0;
};
}
