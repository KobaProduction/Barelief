// 'same' cropping follows madmom audio/signal.py smooth at revision
// 27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/convolution.h>
#include "reference_convolution_fma.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <type_traits>
#if defined(__MINGW32__) && (defined(__i386__) || defined(__x86_64__))
#include <immintrin.h>
#endif
namespace dsp_kernels {
namespace detail {
namespace {
#if defined(__MINGW32__) && (defined(__i386__) || defined(__x86_64__))
__attribute__((target("avx,fma")))
#endif
double reference_convolution_fma(double left,double right,double addend) noexcept {
#if defined(__MINGW32__) && (defined(__i386__) || defined(__x86_64__))
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(left),_mm_set_sd(right),_mm_set_sd(addend)));
#else
    return std::fma(left,right,addend);
#endif
}
}
ReferenceConvolutionFma select_reference_convolution_fma() noexcept {
#if defined(__MINGW32__) && (defined(__i386__) || defined(__x86_64__))
    // GCC's AVX feature includes OSXSAVE/XGETBV validation of SSE and YMM state.
    if(!__builtin_cpu_supports("avx") || !__builtin_cpu_supports("fma"))return nullptr;
#endif
    return reference_convolution_fma;
}
}
namespace {
template<typename Scalar> bool finite(std::span<const Scalar> input) noexcept {
    return std::all_of(input.begin(),input.end(),[](Scalar x){return std::isfinite(x);});
}
template<typename Scalar> bool overlap(std::span<const Scalar> input,std::span<Scalar> output) noexcept {
    if(input.empty() || output.empty())return false;
    const std::less<const Scalar*> less;
    return less(input.data(),output.data()+output.size()) && less(output.data(),input.data()+input.size());
}
template<typename Scalar> std::size_t term_index(std::size_t term,std::size_t input,std::size_t kernel) noexcept {
    if constexpr(std::is_same_v<Scalar,double>)return input>=kernel?kernel-1-term:term;
    return term;
}
// Double reduction order adapted from OpenBLAS c2f4bdbbb43a1d20a7342f40122e18e573ce436a,
// kernel/x86_64/ddot.c and ddot_microk_haswell-2.c. Copyright (c) 2014,
// The OpenBLAS Project. BSD-3-Clause notice: packaging/licenses/openblas.txt.
struct ReferenceDot {
    std::span<const double> forward,reverse;
    double left(std::size_t i) const noexcept {return forward[i];}
    double right(std::size_t i) const noexcept {return reverse[reverse.size()-1-i];}
};
ReferenceDot reference_overlap(std::span<const double> input,std::span<const double> kernel,std::ptrdiff_t at) noexcept {
    if(at<0)return {};
    const auto longer=input.size()>=kernel.size()?input:kernel;
    const auto shorter=input.size()>=kernel.size()?kernel:input;
    const auto position=std::size_t(at);
    const auto begin=position>=shorter.size()?position-(shorter.size()-1):0;
    const auto end=std::min(longer.size(),position+1);
    if(begin>=end)return {};
    const auto count=end-begin;
    return {longer.subspan(begin,count),shorter.subspan(position-(end-1),count)};
}
double rounded_reference_product(double left,double right) noexcept {
    volatile double product=left*right;return product;
}
double reference_scalar_tail(ReferenceDot dot,std::size_t first,double sum) noexcept {
    for(std::size_t i=first;i<dot.forward.size();++i)sum+=rounded_reference_product(dot.left(i),dot.right(i));
    return sum;
}
double reference_lane_half(const std::array<double,16>& lanes,std::size_t parity) noexcept {
    return ((lanes[parity]+lanes[parity+2])+(lanes[parity+4]+lanes[parity+6]))+
        ((lanes[parity+8]+lanes[parity+10])+(lanes[parity+12]+lanes[parity+14]));
}
double reference_ordered_dot(ReferenceDot dot,detail::ReferenceConvolutionFma fused) noexcept {
    const auto prefix=dot.forward.size()&~std::size_t{15};
    if(!prefix)return reference_scalar_tail(dot,0,0.);
    std::array<double,16> lanes{};
    for(std::size_t offset=0;offset<prefix;offset+=lanes.size())
        for(std::size_t lane=0;lane<lanes.size();++lane)
            lanes[lane]=fused(dot.left(offset+lane),dot.right(offset+lane),lanes[lane]);
    const double sum=reference_lane_half(lanes,0)+reference_lane_half(lanes,1);
    return reference_scalar_tail(dot,prefix,sum);
}
template<typename Scalar> Scalar coefficient(std::span<const Scalar> input,std::span<const Scalar> kernel,std::ptrdiff_t at,detail::ReferenceConvolutionFma fused) noexcept {
    if constexpr(std::is_same_v<Scalar,double>)return reference_ordered_dot(reference_overlap(input,kernel,at),fused);
    Scalar value=0;
    for(std::size_t term=0;term<kernel.size();++term){
        const auto k=term_index<Scalar>(term,input.size(),kernel.size());
        const auto source=at-std::ptrdiff_t(k);
        if(source>=0 && std::size_t(source)<input.size())value+=input[std::size_t(source)]*kernel[k];
    }
    return value;
}
template<typename Scalar> bool render_convolution(std::span<const Scalar> input,std::span<const Scalar> kernel,std::span<Scalar> output,SameConvolutionCrop crop,detail::ReferenceConvolutionFma fused) noexcept {
    const auto center=crop==SameConvolutionCrop::Input?kernel.size():std::min(input.size(),kernel.size());
    const auto offset=std::ptrdiff_t((center-1)/2);
    for(std::size_t i=0;i<output.size();++i){
        const Scalar value=coefficient(input,kernel,std::ptrdiff_t(i)+offset,fused);
        if(!std::isfinite(value))return false;
        output[i]=value;
    }
    return true;
}
template<typename Scalar> bool convolve(std::span<const Scalar> input,std::span<const Scalar> kernel,
                                       std::span<Scalar> output,SameConvolutionCrop crop) noexcept {
    if(unsigned(crop)>unsigned(SameConvolutionCrop::Input) || input.size()>std::size_t(PTRDIFF_MAX)/sizeof(Scalar) ||
       kernel.size()>std::size_t(PTRDIFF_MAX)/sizeof(Scalar) || output.size()!=same_convolution_size(input.size(),kernel.size(),crop))return false;
    if(!finite(input) || !finite(kernel) || overlap(input,output) || overlap(kernel,output))return false;
    if(input.empty())return true;
    if(kernel.empty()){std::copy(input.begin(),input.end(),output.begin());return true;}
    detail::ReferenceConvolutionFma fused=nullptr;
    if constexpr(std::is_same_v<Scalar,double>){
        fused=detail::select_reference_convolution_fma();
        if(std::min(input.size(),kernel.size())>=16 && !fused)return false;
    }
    return render_convolution(input,kernel,output,crop,fused);
}
}
std::size_t same_convolution_size(std::size_t input,std::size_t kernel,SameConvolutionCrop crop) noexcept {
    if(unsigned(crop)>unsigned(SameConvolutionCrop::Input))return 0;
    return input?(crop==SameConvolutionCrop::Input?input:std::max(input,kernel)):0;
}
bool convolve_same(std::span<const float> input,std::span<const float> kernel,std::span<float> output,
                   SameConvolutionCrop crop) noexcept {
    return convolve(input,kernel,output,crop);
}
bool convolve_same(std::span<const double> input,std::span<const double> kernel,std::span<double> output,
                   SameConvolutionCrop crop) noexcept {
    return convolve(input,kernel,output,crop);
}
}
