#pragma once
#include <cstddef>
#include <span>

namespace dsp_kernels {
enum class ScalarReduction { MaximumAbsolute, SignedMean, Rms };
enum class ScalarReduceStatus {
    Ok, InvalidOperation, EmptyInput, LimitExceeded, NonFiniteInput, NonFiniteResult
};
struct ScalarReduceLimits { std::size_t max_values; };
// Both spans form one population. Empty/error leaves output unchanged.
// Caller selects channels/newest history row and supplies an effective limit.
ScalarReduceStatus scalar_reduce(ScalarReduction operation,
    std::span<const float> first, std::span<const float> second,
    ScalarReduceLimits limits, float& output) noexcept;
}
