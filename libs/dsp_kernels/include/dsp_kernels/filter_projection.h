#pragma once
#include <cmath>
#include <span>
namespace dsp_kernels {
struct FilterProjectionOptions {bool compensated=true,square_input=false;};
namespace detail {
inline void accumulate_filter_product(float product,float& sum,float& error,bool compensated) noexcept {
    if(compensated){
        const float term=product-error,next=sum+term;
        error=(next-sum)-term;sum=next;
    }else sum+=product;
}
}
// Shared Float32 dot product for explicit filter coefficients.
inline bool project_filter_weights(std::span<const float> input,std::span<const float> weights,float& output,
                                   FilterProjectionOptions options={}) noexcept {
    if(input.size()!=weights.size())return false;
    float sum=0,error=0;
    for(std::size_t i=0;i<input.size();++i){
        const float value=options.square_input?input[i]*input[i]:input[i];
        const float product=value*weights[i];
        detail::accumulate_filter_product(product,sum,error,options.compensated);
    }
    if(!std::isfinite(sum))return false;
    output=sum;return true;
}
struct FilterBandSupport {
    std::size_t first=0,count=0;
};
}
