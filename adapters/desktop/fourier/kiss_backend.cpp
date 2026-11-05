// SPDX-License-Identifier: BSD-3-Clause
// Adapter for pinned KISS FFT130; vendor/PIN.json records source and storage patch.
#include "kiss_backend.h"
#include "kiss_backend_symbols.h"
#include <dsp_kernels/dsp.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
extern "C" {
#include "kiss_fft.h"
#include "kiss_fftr.h"
}

namespace desktop_fourier {
namespace {
using namespace dsp_kernels;
static_assert(std::is_same_v<kiss_fft_scalar,float>);
static_assert(sizeof(kiss_fft_cpx)==2*sizeof(float));
bool real_transform(FourierBackendTransform t) noexcept {
    return t==FourierBackendTransform::RealForward || t==FourierBackendTransform::RealInverse;
}
bool inverse_transform(FourierBackendTransform t) noexcept {
    return t==FourierBackendTransform::RealInverse || t==FourierBackendTransform::ComplexInverse;
}
struct Shape {std::size_t input=0,output=0,plan=0;};
bool shape(FourierBackendRequest request,Shape& value) noexcept {
    const auto n=request.samples;
    if(!fourier_backend_sizes(request,value.input,value.output) || n>max_fft_samples || n>16384)return false;
    std::size_t bytes=0;
    if(real_transform(request.transform))kiss_fftr_alloc(int(n),int(inverse_transform(request.transform)),nullptr,&bytes);
    else kiss_fft_alloc(int(n),int(inverse_transform(request.transform)),nullptr,&bytes);
    constexpr auto alignment=alignof(std::max_align_t);
    value.plan=(bytes+alignment-1)/alignment*alignment;return value.plan!=0;
}
struct PlanDelete {
    void operator()(std::byte* p) const noexcept {::operator delete(p,std::align_val_t{alignof(std::max_align_t)});}
};
class KissBackend final:public FourierBackend {
public:
    KissBackend(FourierBackendRequest request,Shape sizes):request_(request),sizes_(sizes) {}
    bool prepare();
    bool process(std::span<const float>,std::span<float>) noexcept override;
    FourierBackendResources resources() const noexcept override;
    std::unique_ptr<FourierBackend> clone() const override;
private:
    void prepare_buffers();
    void load(std::span<const float>) noexcept;
    void execute() noexcept;
    void normalize() noexcept;
    bool finite() const noexcept;
    void publish(std::span<float>) const noexcept;
    FourierBackendRequest request_;
    Shape sizes_;
    std::unique_ptr<std::byte,PlanDelete> plan_;
    std::unique_ptr<float[]> real_input_,real_output_;
    std::unique_ptr<kiss_fft_cpx[]> complex_input_,complex_output_;
};
FourierBackendResources storage(Shape s) noexcept {
    const auto owned=sizeof(KissBackend)+s.plan+(s.input+s.output)*sizeof(float);
    // N<=16384 has at most14 recursive FFT levels. Generic radix scratch is
    // part of the cold plan; compiler stack evidence is recorded separately.
    return {owned,owned,0,16384};
}
void KissBackend::prepare_buffers() {
    if(request_.transform==FourierBackendTransform::RealForward){
        real_input_=std::make_unique<float[]>(request_.samples);
        complex_output_=std::make_unique<kiss_fft_cpx[]>(request_.samples/2+1);
    }else if(request_.transform==FourierBackendTransform::RealInverse){
        complex_input_=std::make_unique<kiss_fft_cpx[]>(request_.samples/2+1);
        real_output_=std::make_unique<float[]>(request_.samples);
    }else{
        complex_input_=std::make_unique<kiss_fft_cpx[]>(request_.samples);
        complex_output_=std::make_unique<kiss_fft_cpx[]>(request_.samples);
    }
}
bool KissBackend::prepare() {
    plan_.reset(static_cast<std::byte*>(::operator new(sizes_.plan,std::align_val_t{alignof(std::max_align_t)})));
    std::size_t bytes=sizes_.plan;void* configured=nullptr;
    if(real_transform(request_.transform))configured=kiss_fftr_alloc(int(request_.samples),int(inverse_transform(request_.transform)),plan_.get(),&bytes);
    else configured=kiss_fft_alloc(int(request_.samples),int(inverse_transform(request_.transform)),plan_.get(),&bytes);
    if(!configured)return false;
    prepare_buffers();return true;
}
void KissBackend::load(std::span<const float> input) noexcept {
    if(real_input_){std::copy(input.begin(),input.end(),real_input_.get());return;}
    for(std::size_t i=0;i<input.size()/2;++i)complex_input_[i]={input[2*i],input[2*i+1]};
}
void KissBackend::execute() noexcept {
    if(request_.transform==FourierBackendTransform::RealForward)
        kiss_fftr(reinterpret_cast<kiss_fftr_cfg>(plan_.get()),real_input_.get(),complex_output_.get());
    else if(request_.transform==FourierBackendTransform::RealInverse)
        kiss_fftri(reinterpret_cast<kiss_fftr_cfg>(plan_.get()),complex_input_.get(),real_output_.get());
    else kiss_fft(reinterpret_cast<kiss_fft_cfg>(plan_.get()),complex_input_.get(),complex_output_.get());
}
void KissBackend::normalize() noexcept {
    if(!inverse_transform(request_.transform))return;
    const float divisor=float(request_.samples);
    if(real_output_)for(std::size_t i=0;i<request_.samples;++i)real_output_[i]/=divisor;
    else for(std::size_t i=0;i<request_.samples;++i){complex_output_[i].r/=divisor;complex_output_[i].i/=divisor;}
}
bool KissBackend::finite() const noexcept {
    if(real_output_)return std::all_of(real_output_.get(),real_output_.get()+sizes_.output,[](float v){return std::isfinite(v);});
    return std::all_of(complex_output_.get(),complex_output_.get()+sizes_.output/2,
        [](kiss_fft_cpx v){return std::isfinite(v.r) && std::isfinite(v.i);});
}
void KissBackend::publish(std::span<float> output) const noexcept {
    if(real_output_){std::copy_n(real_output_.get(),output.size(),output.begin());return;}
    for(std::size_t i=0;i<output.size()/2;++i){output[2*i]=complex_output_[i].r;output[2*i+1]=complex_output_[i].i;}
}
bool KissBackend::process(std::span<const float> input,std::span<float> output) noexcept {
    if(!plan_ || !fourier_backend_call_valid(request_,input,output))return false;
    load(input);execute();normalize();
    if(!finite())return false;
    publish(output);return true;
}
FourierBackendResources KissBackend::resources() const noexcept {return storage(sizes_);}
std::unique_ptr<FourierBackend> KissBackend::clone() const {
    auto result=std::make_unique<KissBackend>(request_,sizes_);
    return result->prepare()?std::unique_ptr<FourierBackend>(std::move(result)):nullptr;
}
class Factory final:public FourierBackendFactory {
public:
    const char* id() const noexcept override {return "kiss_fft130_float32";}
    bool query(FourierBackendRequest request,FourierBackendResources& output) const noexcept override {
        Shape sizes;if(!shape(request,sizes))return false;output=storage(sizes);return true;
    }
    std::unique_ptr<FourierBackend> prepare(FourierBackendRequest request) const override {
        Shape sizes;if(!shape(request,sizes))return {};
        auto result=std::make_unique<KissBackend>(request,sizes);
        return result->prepare()?std::unique_ptr<FourierBackend>(std::move(result)):nullptr;
    }
};
}
const dsp_kernels::FourierBackendFactory& kiss_backend_factory() noexcept {static const Factory factory;return factory;}
}
