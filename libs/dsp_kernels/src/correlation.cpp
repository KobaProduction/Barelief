#include <dsp_kernels/correlation.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdint>

namespace dsp_kernels {
namespace {
bool overlapping(std::span<const float> input,std::span<float> output) noexcept {
    if(input.empty() || output.empty())return false;
    const std::less<const float*> less;
    return less(input.data(),output.data()+output.size()) && less(output.data(),input.data()+input.size());
}
struct Products {float dot=0,left=0,right=0;};
Products products(std::span<const float> left,std::span<const float> right) noexcept {
    Products result;
    for(std::size_t i=0;i<left.size();++i){
        const float a=left[i],b=right[i];
        result.dot+=a*b;result.left+=a*a;result.right+=b*b;
    }
    return result;
}
float overlap_energy(Products p,OverlapEnergyPolicy policy) noexcept {
    if(p.left<=policy.energy_floor || p.right<=policy.energy_floor)return policy.rejected_value;
    const float divisor=policy.multiply_energies?std::sqrt(p.left*p.right):std::sqrt(p.left)*std::sqrt(p.right);
    return std::clamp(p.dot/divisor,policy.minimum,policy.maximum);
}
float normalize(Products p,std::size_t overlap,std::size_t total,CorrelationNormalization mode) noexcept {
    if(mode==CorrelationNormalization::Raw)return p.dot;
    if(mode==CorrelationNormalization::Unbiased)return p.dot/float(overlap);
    if(mode==CorrelationNormalization::Biased)return p.dot/float(total);
    return overlap_energy(p,{});
}
float cross(std::span<const float> left,std::span<const float> right,std::ptrdiff_t lag,CorrelationNormalization mode) noexcept {
    const auto shift=std::size_t(lag<0?-lag:lag),count=left.size()-shift;
    return normalize(products(left.subspan(lag>0?shift:0,count),right.subspan(lag<0?shift:0,count)),count,left.size(),mode);
}
float coefficient(std::span<const float> input,std::size_t lag,CorrelationNormalization mode) noexcept {
    return normalize(products(input.subspan(lag),input.first(input.size()-lag)),input.size()-lag,input.size(),mode);
}
bool finite(std::span<const float> input) noexcept {
    return std::all_of(input.begin(),input.end(),[](float value){return std::isfinite(value);});
}
bool valid(OverlapEnergyPolicy policy) noexcept {
    return std::isfinite(policy.energy_floor) && policy.energy_floor>=0 &&
        std::isfinite(policy.rejected_value) && std::isfinite(policy.minimum) &&
        std::isfinite(policy.maximum) && policy.minimum<=policy.maximum;
}
float overlap_coefficient(std::span<const float> input,std::size_t lag,OverlapEnergyPolicy policy) noexcept {
    return overlap_energy(products(input.subspan(lag),input.first(input.size()-lag)),policy);
}
}
bool overlap_energy_autocorrelation(std::span<const float> input,std::size_t first,std::span<float> output,OverlapEnergyPolicy policy) noexcept {
    if(input.empty() || first>=input.size() || output.size()>input.size()-first)return false;
    if(!valid(policy) || overlapping(input,output) || !finite(input))return false;
    for(std::size_t i=0;i<output.size();++i){
        const float value=overlap_coefficient(input,first+i,policy);
        if(!std::isfinite(value))return false;
        output[i]=value;
    }
    return true;
}
bool linear_crosscorrelation(std::span<const float> left,std::span<const float> right,std::ptrdiff_t first,std::span<float> output,CorrelationNormalization mode) noexcept {
    if(left.empty() || left.size()!=right.size() || left.size()>std::size_t(PTRDIFF_MAX)/2)return false;
    const auto limit=std::ptrdiff_t(left.size()-1);
    if(first<-limit || first>limit || output.size()>std::size_t(limit-first)+1)return false;
    if(mode<CorrelationNormalization::Raw || mode>CorrelationNormalization::OverlapEnergy)return false;
    if(overlapping(left,output) || overlapping(right,output) || !finite(left) || !finite(right))return false;
    for(std::size_t i=0;i<output.size();++i)output[i]=cross(left,right,first+std::ptrdiff_t(i),mode);
    return finite(output);
}
bool linear_autocorrelation(std::span<const float> input,std::span<float> output) noexcept {
    return linear_autocorrelation(input,output,CorrelationNormalization::OverlapEnergy);
}
bool linear_autocorrelation(std::span<const float> input,std::span<float> output,CorrelationNormalization normalization) noexcept {
    if(output.size()>input.size() || overlapping(input,output))return false;
    if(normalization<CorrelationNormalization::Raw || normalization>CorrelationNormalization::OverlapEnergy)return false;
    if(!std::all_of(input.begin(),input.end(),[](float value){return std::isfinite(value);}))return false;
    for(std::size_t lag=0;lag<output.size();++lag)output[lag]=coefficient(input,lag,normalization);
    if(!std::all_of(output.begin(),output.end(),[](float value){return std::isfinite(value);}))return false;
    return true;
}
}
