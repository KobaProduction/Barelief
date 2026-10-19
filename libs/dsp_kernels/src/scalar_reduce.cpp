#include <dsp_kernels/scalar_reduce.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace dsp_kernels {
namespace {
struct ScalarAccumulator {
    ScalarReduction operation;double sum=0;float maximum=0;
    bool append(std::span<const float> values)noexcept {
        for(float value:values){
            if(!std::isfinite(value))return false;
            maximum=std::max(maximum,std::abs(value));
            if(operation==ScalarReduction::SignedMean)sum+=double(value);
            else if(operation==ScalarReduction::Rms)sum+=double(value)*double(value);
        }
        return true;
    }
    double value(std::size_t count)const noexcept {
        if(operation==ScalarReduction::MaximumAbsolute)return double(maximum);
        return operation==ScalarReduction::Rms?std::sqrt(sum/double(count)):sum/double(count);
    }
};
ScalarReduceStatus publish_scalar(double value,float& output)noexcept {
    if(!std::isfinite(value)||std::abs(value)>double(std::numeric_limits<float>::max()))return ScalarReduceStatus::NonFiniteResult;
    const auto result=float(value);if(!std::isfinite(result))return ScalarReduceStatus::NonFiniteResult;
    output=result;return ScalarReduceStatus::Ok;
}
ScalarReduceStatus validate_scalar_input(ScalarReduction operation,std::span<const float> first,
                                         std::span<const float> second,ScalarReduceLimits limits) noexcept {
    if(operation!=ScalarReduction::MaximumAbsolute && operation!=ScalarReduction::SignedMean &&
       operation!=ScalarReduction::Rms)return ScalarReduceStatus::InvalidOperation;
    if(first.size()>std::numeric_limits<std::size_t>::max()-second.size())return ScalarReduceStatus::LimitExceeded;
    const auto count=first.size()+second.size();
    if(!count)return ScalarReduceStatus::EmptyInput;
    return count>limits.max_values?ScalarReduceStatus::LimitExceeded:ScalarReduceStatus::Ok;
}
}
ScalarReduceStatus scalar_reduce(ScalarReduction operation,
    std::span<const float> first, std::span<const float> second,
    ScalarReduceLimits limits, float& output) noexcept {
    const auto admission=validate_scalar_input(operation,first,second,limits);
    if(admission!=ScalarReduceStatus::Ok)return admission;
    const auto count = first.size() + second.size();
    ScalarAccumulator accumulator{operation};
    for(const auto values:{first,second})if(!accumulator.append(values))return ScalarReduceStatus::NonFiniteInput;
    return publish_scalar(accumulator.value(count),output);
}
}
