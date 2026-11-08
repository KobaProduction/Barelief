#include <dsp_kernels/sequence_statistics.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>

namespace dsp_kernels {
namespace {
struct CenteredMoments {
    float xx=0,xy=0,yy=0;
    void add(float x,float y) noexcept {
        xx+=x*x;xy+=x*y;yy+=y*y;
    }
};
bool publish(float value,float& output) noexcept {
    if(!std::isfinite(value))return false;
    output=value;return true;
}
}
bool log_sum_exp_float32(std::span<const float> values,float& output) noexcept {
    if(values.empty() || !std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x) || x==-std::numeric_limits<float>::infinity();}))return false;
    const float maximum=*std::max_element(values.begin(),values.end());
    if(!std::isfinite(maximum))return false;
    float sum=0;
    for(const float value:values)sum+=std::exp(value-maximum);
    return publish(std::log(sum)+maximum,output);
}
bool sum_float32(std::span<const float> values,float& output,Float32Summation mode,bool nonfinite) noexcept {
    if(unsigned(mode)>unsigned(Float32Summation::Compensated) ||
       (!nonfinite && !std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x); })))return false;
    Float32Accumulator sum;
    for(const float value:values)sum.add(value,mode);
    if(!nonfinite)return publish(sum.value,output);
    output=sum.value;return true;
}
bool arithmetic_mean_float32(std::span<const float> values,float& output,Float32Summation mode) noexcept {
    float sum=0;
    return !values.empty() && sum_float32(values,sum,mode) && publish(sum/float(values.size()),output);
}
bool accumulate_scaled_mean_float32(float value,float count,float& sum,float& correction) noexcept {
    if(!std::isfinite(value) || !std::isfinite(count) || count<=0)return false;
    Float32Accumulator accumulator{sum,correction};accumulator.add(value/count);
    if(!std::isfinite(accumulator.value) || !std::isfinite(accumulator.error))return false;
    sum=accumulator.value;correction=accumulator.error;return true;
}
bool rayleigh_weights(std::span<float> values,float scale) noexcept {
    if(!std::isfinite(scale) || scale<=0 || !std::isfinite(scale*scale))return false;
    const float variance=scale*scale;
    for(std::size_t i=0;i<values.size();++i){
        const float tau=float(i+1);values[i]=tau/variance*std::exp(-.5f*tau*tau/variance);
    }
    return std::all_of(values.begin(),values.end(),[](float value){return std::isfinite(value);});
}
bool variance_float32(std::span<const float> values,float& output,std::size_t correction,Float32Summation mode,bool count_divisor) noexcept {
    float mean=0;
    if(values.size()<=correction || !arithmetic_mean_float32(values,mean,mode))return false;
    Float32Accumulator sum;
    for(const float value:values){const float delta=value-mean;sum.add(delta*delta,mode);}
    const float divisor=count_divisor?float(values.size()-correction):float(values.size())-float(correction);
    return publish(sum.value/divisor,output);
}
bool sample_deviation_float32(std::span<const float> values,float& output,Float32Summation mode,bool count_divisor) noexcept {
    float variance=0;
    return variance_float32(values,variance,1,mode,count_divisor) && publish(std::sqrt(variance),output);
}
bool linear_residual_mean_square(std::span<const float> values,float& output) noexcept {
    float mean=0;
    if(values.size()<2 || !arithmetic_mean_float32(values,mean))return false;
    const float center=(float(values.size())-1.f)*.5f;
    CenteredMoments sums;
    for(std::size_t i=0;i<values.size();++i)sums.add(float(i)-center,values[i]-mean);
    return publish((sums.yy-sums.xy*sums.xy/sums.xx)/float(values.size()),output);
}
}
