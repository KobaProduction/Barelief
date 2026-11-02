#include <dsp_kernels/fourier_transform.h>
#include <dsp_kernels/detail/fourier_geometry.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
#include <utility>

namespace dsp_kernels {
std::size_t PreparedFourierTransform::workspace_bytes() const noexcept {
    return (real_.capacity()+imaginary_.capacity()+transformed_.capacity()+result_.capacity()+chirp_.capacity()+kernel_.capacity())*sizeof(float)+
        fft_.workspace_bytes()+blocked_.workspace_bytes()+forward_backend_.workspace_bytes()+inverse_backend_.workspace_bytes();
}
namespace {
bool finite(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float value){return std::isfinite(value);});
}

}
bool PreparedFourierTransform::prepare(std::size_t samples,const FourierBackendFactory* backend) {
    std::size_t bound=0;if(!storage_bytes(samples,bound,backend))return false;
    PreparedFourierTransform next;
    if(!(backend?next.configure_backend(samples,*backend):next.configure(samples)) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedFourierTransform::configure_backend(std::size_t samples,const FourierBackendFactory& factory) {
    if(!forward_backend_.prepare({samples,FourierBackendTransform::ComplexForward},factory) ||
       !inverse_backend_.prepare({samples,FourierBackendTransform::ComplexInverse},factory))return false;
    samples_=capacity_=samples;transformed_.resize(2*samples);result_.resize(2*samples);return true;
}
bool PreparedFourierTransform::configure(std::size_t samples) {
    capacity_=detail::fourier_convolution_size(samples);if(!capacity_)return false;
    samples_=samples;chirped_=samples!=capacity_;
    real_.resize(capacity_);imaginary_.resize(capacity_);
    transformed_.resize(2*capacity_);result_.resize(2*samples_);
    if(capacity_>max_fft_samples){if(!blocked_.prepare(capacity_))return false;}
    else if(capacity_>=4 && !fft_.prepare(capacity_))return false;
    return !chirped_ || prepare_chirp();
}
bool PreparedFourierTransform::transform(std::span<const float> real,std::span<const float> imaginary,std::span<float> output) noexcept {
    return capacity_>max_fft_samples?blocked_.process(real,imaginary,output):fft_.process_complex(real,imaginary,output);
}
bool PreparedFourierTransform::prepare_chirp() {
    chirp_.resize(2*samples_);kernel_.resize(2*capacity_);
    for(std::size_t i=0;i<samples_;++i){
        const double angle=std::numbers::pi*double(i)*double(i)/double(samples_);
        const float re=float(std::cos(angle)),im=float(std::sin(angle));
        chirp_[2*i]=re;chirp_[2*i+1]=-im;
        real_[i]=re;imaginary_[i]=im;
        if(i){real_[capacity_-i]=re;imaginary_[capacity_-i]=im;}
    }
    return transform(real_,imaginary_,kernel_);
}
void PreparedFourierTransform::load(std::span<const float> re,std::span<const float> im,bool inverse) noexcept {
    std::fill(real_.begin(),real_.end(),0.f);std::fill(imaginary_.begin(),imaginary_.end(),0.f);
    for(std::size_t i=0;i<samples_;++i){
        const float a=re[i],b=im.empty()?0.f:(inverse?-im[i]:im[i]);
        const float c=chirped_?chirp_[2*i]:1.f,d=chirped_?chirp_[2*i+1]:0.f;
        real_[i]=a*c-b*d;imaginary_[i]=a*d+b*c;
    }
}
bool PreparedFourierTransform::tiny() noexcept {
    result_[0]=real_[0];result_[1]=imaginary_[0];
    if(samples_==2){
        result_[0]+=real_[1];result_[1]+=imaginary_[1];
        result_[2]=real_[0]-real_[1];result_[3]=imaginary_[0]-imaginary_[1];
    }
    return finite(result_);
}
bool PreparedFourierTransform::convolve() noexcept {
    for(std::size_t i=0;i<capacity_;++i){
        const float a=transformed_[2*i],b=transformed_[2*i+1],c=kernel_[2*i],d=kernel_[2*i+1];
        real_[i]=a*c-b*d;imaginary_[i]=-(a*d+b*c);
    }
    if(!transform(real_,imaginary_,transformed_))return false;
    for(std::size_t i=0;i<samples_;++i){
        const float a=transformed_[2*i]/float(capacity_),b=-transformed_[2*i+1]/float(capacity_);
        result_[2*i]=a*chirp_[2*i]-b*chirp_[2*i+1];
        result_[2*i+1]=a*chirp_[2*i+1]+b*chirp_[2*i];
    }
    return finite(result_);
}
bool PreparedFourierTransform::forward() noexcept {
    if(capacity_<4)return tiny();
    if(!transform(real_,imaginary_,transformed_))return false;
    if(chirped_)return convolve();
    std::copy_n(transformed_.begin(),result_.size(),result_.begin());return finite(result_);
}
void PreparedFourierTransform::finish(std::span<float> output,bool inverse) noexcept {
    const float scale=inverse?1.f/float(samples_):1.f;
    for(std::size_t i=0;i<samples_;++i){output[2*i]=result_[2*i]*scale;output[2*i+1]=result_[2*i+1]*(inverse?-scale:scale);}
}
bool PreparedFourierTransform::process(std::span<const float> re,std::span<const float> im,std::span<float> output,bool inverse) noexcept {
    if(!samples_ || re.size()!=samples_ || (!im.empty() && im.size()!=samples_) || output.size()!=2*samples_)return false;
    if(!finite(re) || !finite(im))return false;
    if(forward_backend_.ready())return process_backend(re,im,output,inverse);
    load(re,im,inverse);if(!forward())return false;
    finish(output,inverse);return true;
}
bool PreparedFourierTransform::process_backend(std::span<const float> re,std::span<const float> im,std::span<float> output,bool inverse) noexcept {
    for(std::size_t i=0;i<samples_;++i){transformed_[2*i]=re[i];transformed_[2*i+1]=im.empty()?0.f:im[i];}
    auto& backend=inverse?inverse_backend_:forward_backend_;
    if(!backend.process(transformed_,result_))return false;
    std::copy(result_.begin(),result_.end(),output.begin());return true;
}
}
