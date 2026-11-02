// 'same' cropping follows madmom audio/signal.py smooth at revision
// 27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/convolution.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
namespace dsp_kernels {
namespace {
bool finite(std::span<const float> input) noexcept {
    return std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);});
}
bool overlap(std::span<const float> input,std::span<float> output) noexcept {
    if(input.empty() || output.empty())return false;
    const std::less<const float*> less;
    return less(input.data(),output.data()+output.size()) && less(output.data(),input.data()+input.size());
}
float coefficient(std::span<const float> input,std::span<const float> kernel,std::ptrdiff_t at) noexcept {
    float value=0;
    for(std::size_t k=0;k<kernel.size();++k){
        const auto source=at-std::ptrdiff_t(k);
        if(source>=0 && std::size_t(source)<input.size())value+=input[std::size_t(source)]*kernel[k];
    }
    return value;
}
}
std::size_t same_convolution_size(std::size_t input,std::size_t kernel,SameConvolutionCrop crop) noexcept {
    if(unsigned(crop)>unsigned(SameConvolutionCrop::Input))return 0;
    return input?(crop==SameConvolutionCrop::Input?input:std::max(input,kernel)):0;
}
bool convolve_same(std::span<const float> input,std::span<const float> kernel,std::span<float> output,
                   SameConvolutionCrop crop) noexcept {
    if(unsigned(crop)>unsigned(SameConvolutionCrop::Input) || input.size()>std::size_t(PTRDIFF_MAX)/4 ||
       kernel.size()>std::size_t(PTRDIFF_MAX)/4 || output.size()!=same_convolution_size(input.size(),kernel.size(),crop))return false;
    if(!finite(input) || !finite(kernel) || overlap(input,output) || overlap(kernel,output))return false;
    if(input.empty())return true;
    if(kernel.empty()){std::copy(input.begin(),input.end(),output.begin());return true;}
    const auto center=crop==SameConvolutionCrop::Input?kernel.size():std::min(input.size(),kernel.size());
    const auto offset=std::ptrdiff_t((center-1)/2);
    for(std::size_t i=0;i<output.size();++i){
        const float value=coefficient(input,kernel,std::ptrdiff_t(i)+offset);
        if(!std::isfinite(value))return false;
        output[i]=value;
    }
    return true;
}
}
