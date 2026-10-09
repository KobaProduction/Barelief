#include <dsp_kernels/scalar_reduce.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace dsp_kernels {
ScalarReduceStatus scalar_reduce(ScalarReduction operation,
    std::span<const float> first, std::span<const float> second,
    ScalarReduceLimits limits, float& output) noexcept {
    if (operation != ScalarReduction::MaximumAbsolute && operation != ScalarReduction::SignedMean &&
        operation != ScalarReduction::Rms) return ScalarReduceStatus::InvalidOperation;
    if (first.size() > std::numeric_limits<std::size_t>::max() - second.size())
        return ScalarReduceStatus::LimitExceeded;
    const auto count = first.size() + second.size();
    if (!count) return ScalarReduceStatus::EmptyInput;
    if (count > limits.max_values) return ScalarReduceStatus::LimitExceeded;
    double sum = 0;
    float maximum = 0;
    for (const auto values : {first, second}) {
        for (float value : values) {
            if (!std::isfinite(value)) return ScalarReduceStatus::NonFiniteInput;
            maximum = std::max(maximum, std::abs(value));
            if (operation == ScalarReduction::SignedMean) sum += double(value);
            else if (operation == ScalarReduction::Rms) sum += double(value) * double(value);
        }
    }
    const double reduced = operation == ScalarReduction::MaximumAbsolute ? double(maximum) :
        operation == ScalarReduction::Rms ? std::sqrt(sum / double(count)) : sum / double(count);
    if (!std::isfinite(reduced) || std::abs(reduced) > double(std::numeric_limits<float>::max()))
        return ScalarReduceStatus::NonFiniteResult;
    const auto result = float(reduced);
    if (!std::isfinite(result)) return ScalarReduceStatus::NonFiniteResult;
    output = result;
    return ScalarReduceStatus::Ok;
}
}
