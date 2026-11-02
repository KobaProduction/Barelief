#include <dsp_kernels/detail/blocked_fourier.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace dsp_kernels::detail {
namespace {
std::size_t reverse_block(std::size_t index,std::size_t blocks) noexcept {
    std::size_t result=0;
    for(auto count=blocks;count>1;count>>=1){result=(result<<1)|(index&1);index>>=1;}
    return result;
}
}
bool PreparedBlockedFourier::configure(std::size_t samples) {
    if(samples<=max_fft_samples || !std::has_single_bit(samples) || samples>std::numeric_limits<std::size_t>::max()/4)return false;
    leaf_size_=max_fft_samples;
    if(!leaf_.prepare(leaf_size_))return false;
    samples_=samples;permutation_.resize(samples/leaf_size_);real_.resize(leaf_size_);imaginary_.resize(leaf_size_);
    transformed_.resize(2*leaf_size_);twiddles_.resize(samples);tables();return true;
}
bool PreparedBlockedFourier::prepare(std::size_t samples) {
    std::size_t bound=0;if(!storage_bytes(samples,bound))return false;
    PreparedBlockedFourier next;
    if(!next.configure(samples) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
void PreparedBlockedFourier::tables() {
    for(std::size_t i=0;i<permutation_.size();++i)permutation_[i]=reverse_block(i,permutation_.size());
    for(std::size_t i=0;i<samples_/2;++i){
        const double angle=-2*std::numbers::pi*double(i)/double(samples_);
        twiddles_[2*i]=float(std::cos(angle));twiddles_[2*i+1]=float(std::sin(angle));
    }
}
void PreparedBlockedFourier::load_leaf(std::span<const float> real,std::span<const float> imaginary,std::size_t block) noexcept {
    const auto stride=permutation_.size(),first=permutation_[block];
    for(std::size_t i=0;i<leaf_size_;++i){
        real_[i]=real[first+i*stride];imaginary_[i]=imaginary[first+i*stride];
    }
}
bool PreparedBlockedFourier::leaves(std::span<const float> real,std::span<const float> imaginary,std::span<float> output) noexcept {
    for(std::size_t block=0;block<permutation_.size();++block){
        load_leaf(real,imaginary,block);
        if(!leaf_.process_complex(real_,imaginary_,transformed_))return false;
        std::copy(transformed_.begin(),transformed_.end(),output.begin()+std::ptrdiff_t(2*block*leaf_size_));
    }
    return true;
}
void PreparedBlockedFourier::butterfly(std::span<float> values,std::size_t left,std::size_t right,std::size_t twiddle) const noexcept {
    const float real=values[2*right],imaginary=values[2*right+1],cosine=twiddles_[2*twiddle],sine=twiddles_[2*twiddle+1];
    const float product_real=real*cosine-imaginary*sine,product_imaginary=real*sine+imaginary*cosine;
    const float first_real=values[2*left],first_imaginary=values[2*left+1];
    values[2*left]=first_real+product_real;values[2*left+1]=first_imaginary+product_imaginary;
    values[2*right]=first_real-product_real;values[2*right+1]=first_imaginary-product_imaginary;
}
void PreparedBlockedFourier::merge(std::span<float> values,std::size_t width) const noexcept {
    const auto half=width/2,stride=samples_/width;
    for(std::size_t group=0;group<samples_;group+=width)
        for(std::size_t i=0;i<half;++i)butterfly(values,group+i,group+half+i,i*stride);
}
bool PreparedBlockedFourier::process(std::span<const float> real,std::span<const float> imaginary,std::span<float> output) noexcept {
    if(!samples_ || real.size()!=samples_ || imaginary.size()!=samples_ || output.size()!=2*samples_)return false;
    if(!leaves(real,imaginary,output))return false;
    for(std::size_t width=2*leaf_size_;width<=samples_;width*=2)merge(output,width);
    return std::all_of(output.begin(),output.end(),[](float value){return std::isfinite(value);});
}
}
