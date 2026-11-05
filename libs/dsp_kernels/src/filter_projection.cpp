#include <dsp_kernels/filter_projection.h>
#include <bit>
#include <cstdint>
#include <limits>

namespace dsp_kernels {
namespace {
bool float32_midpoint(double value) noexcept {
    const auto magnitude=std::bit_cast<std::uint64_t>(value)&0x7fffffffffffffffull;
    const int exponent=int((magnitude>>52)&0x7ffu)-1023;
    if(exponent>=-126 && exponent<=127)return (magnitude&0x1fffffffull)==0x10000000ull;
    if(exponent>=-149 && exponent<=-127){
        const auto discarded=unsigned(-97-exponent);
        return (magnitude&((std::uint64_t{1}<<discarded)-1))==(std::uint64_t{1}<<(discarded-1));
    }
    return exponent==-150 && magnitude==std::bit_cast<std::uint64_t>(0x1p-150);
}
float reference_fma32(float left,float right,float addend) noexcept {
    static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<float>::digits==24 && sizeof(float)==4);
    static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits==53 && sizeof(double)==8);
    // Finite binary32 products are exact in binary64. General TwoSum recovers
    // the addition residual; only a binary32 midpoint can double-round in RNE.
    // The predicate includes subnormal and overflow rounding boundaries.
    const double product=double(left)*double(right),sum=product+double(addend);
    if(!float32_midpoint(sum))return float(sum);
    const double shifted=sum-product;
    const double residual=(product-(sum-shifted))+(double(addend)-shifted);
    if(residual==0)return float(sum);
    auto bits=std::bit_cast<std::uint64_t>(sum);
    const bool increase=(residual>0)!=bool(bits>>63);
    bits=increase?bits+1:bits-1;
    return float(std::bit_cast<double>(bits));
}
void accumulate_filter_product(float product,float& sum,float& error,bool compensated) noexcept {
    if(compensated){
        const float term=product-error,next=sum+term;
        error=(next-sum)-term;sum=next;
    }else sum+=product;
}
bool sequential_fma_projection(std::span<const float> input,std::span<const float> weights,float& output,bool square_input) noexcept {
    float sum=0;
    for(std::size_t i=0;i<input.size();++i){
        const float value=square_input?input[i]*input[i]:input[i];
        if(!std::isfinite(value) || !std::isfinite(weights[i]))return false;
        sum=reference_fma32(value,weights[i],sum);
        if(!std::isfinite(sum))return false;
    }
    output=sum;return true;
}
}
bool project_filter_weights(std::span<const float> input,std::span<const float> weights,float& output,
                            FilterProjectionOptions options) noexcept {
    if(input.size()!=weights.size() || !valid_filter_projection_options(options))return false;
    if(options.accumulation==FilterProjectionAccumulation::SequentialFma)return sequential_fma_projection(input,weights,output,options.square_input);
    float sum=0,error=0;
    for(std::size_t i=0;i<input.size();++i){
        const float value=options.square_input?input[i]*input[i]:input[i];
        const float product=value*weights[i];
        accumulate_filter_product(product,sum,error,options.compensated);
    }
    if(!std::isfinite(sum))return false;
    output=sum;return true;
}
}
