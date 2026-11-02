#include <dsp_kernels/value_histogram.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace dsp_kernels {
namespace {
bool overlap(std::span<const float> input,std::span<float> output) noexcept {
    const std::less<const float*> before;
    return before(input.data(),output.data()+output.size()) && before(output.data(),input.data()+input.size());
}
std::size_t bin_for(float value,std::span<const float> centers) noexcept {
    std::size_t first=0,last=centers.size()-1;
    while(first<last){
        const auto middle=first+(last-first)/2;
        const float cutoff=(centers[middle]+centers[middle+1])/2.f;
        if(value>cutoff)first=middle+1;
        else last=middle;
    }
    return first;
}
bool make_centers(std::span<const float> input,std::span<float> centers) noexcept {
    const auto [minimum,maximum]=std::minmax_element(input.begin(),input.end());
    const float width=*maximum-*minimum;
    if(!std::isfinite(width) || width>std::numeric_limits<float>::max()/float(centers.size()))return false;
    for(std::size_t i=0;i<centers.size();++i)centers[i]=(.5f+float(i))*width/float(centers.size())+*minimum;
    return std::all_of(centers.begin(),centers.end(),[](float x){return std::isfinite(x);});
}
}
bool rounded_bin_counts(std::span<const float> input,std::span<std::uint64_t> counts,float step) noexcept {
    if(input.empty() || counts.empty() || !std::isfinite(step) || step<=0)return false;
    for(const float value:input){
        const float index=std::trunc(std::max(value/step,0.f)+.5f);
        if(!std::isfinite(value) || !std::isfinite(index) ||
           index>=float(std::numeric_limits<std::size_t>::max()) || std::size_t(index)>=counts.size())return false;
    }
    std::fill(counts.begin(),counts.end(),0);
    for(const float value:input)++counts[std::size_t(std::max(value/step,0.f)+.5f)];
    return true;
}
bool uniform_value_histogram(std::span<const float> input,std::span<std::uint64_t> counts,std::span<float> centers) noexcept {
    if(input.empty() || counts.size()<2 || counts.size()!=centers.size() || overlap(input,centers))return false;
    if(!std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);}) || !make_centers(input,centers))return false;
    std::fill(counts.begin(),counts.end(),0);
    for(const float value:input)++counts[bin_for(value,centers)];
    return true;
}
}
