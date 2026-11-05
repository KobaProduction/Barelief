// SPDX-License-Identifier: GPL-3.0-or-later
// Pinned aubio RDFT adapter; exact raw provenance/include-only patch in vendor.
#include "ooura_backend.h"
#include "ooura_backend_symbols.h"
#include <dsp_kernels/dsp.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <memory>
extern "C" void aubio_ooura_rdft(int,int,float*,int*,float*);
namespace desktop_fourier {
namespace {
using namespace dsp_kernels;
bool admitted(FourierBackendRequest request) noexcept {
    const auto n=request.samples;
    return n>=2 && n<=max_fft_samples && n<=16384 && !(n&(n-1)) &&
        (request.transform==FourierBackendTransform::RealForward || request.transform==FourierBackendTransform::RealInverse);
}
class OouraBackend final:public FourierBackend {
public:
    explicit OouraBackend(FourierBackendRequest request):request_(request) {}
    bool prepare();
    bool process(std::span<const float>,std::span<float>) noexcept override;
    FourierBackendResources resources() const noexcept override;
    std::unique_ptr<FourierBackend> clone() const override;
private:
    void load(std::span<const float>) noexcept;
    void execute() noexcept;
    void publish(std::span<float>) const noexcept;
    FourierBackendRequest request_;
    std::unique_ptr<float[]> data_,tables_;
    std::unique_ptr<int[]> indices_;
};
bool storage(FourierBackendRequest request,FourierBackendResources& output) noexcept {
    if(!admitted(request))return false;
    std::size_t bytes=sizeof(OouraBackend),length=request.samples/2+1;
    using namespace dsp_kernels::prepared_storage;
    if(!add_array(bytes,request.samples,sizeof(float)) || !add_array(bytes,length,sizeof(int)) ||
       !add_array(bytes,length,sizeof(float)))return false;
    // Source calls are bounded iterative scalar kernels; compiler stack evidence
    // independently validates this ceiling before numerical provider acceptance.
    output={bytes,bytes,0,16384};return true;
}
bool OouraBackend::prepare() {
    if(!admitted(request_))return false;
    data_=std::make_unique<float[]>(request_.samples);
    indices_=std::make_unique<int[]>(request_.samples/2+1);
    tables_=std::make_unique<float[]>(request_.samples/2+1);
    // Materialize both lazy RDFT tables on cold zero input. Hot calls only use
    // this instance's tables and bit-reversal scratch; no source allocations.
    aubio_ooura_rdft(int(request_.samples),1,data_.get(),indices_.get(),tables_.get());
    std::fill_n(data_.get(),request_.samples,0.f);return true;
}
void OouraBackend::load(std::span<const float> input) noexcept {
    if(request_.transform==FourierBackendTransform::RealForward){std::copy(input.begin(),input.end(),data_.get());return;}
    data_[0]=input[0];data_[1]=input[request_.samples];
    for(std::size_t bin=1;bin<request_.samples/2;++bin){
        data_[2*bin]=input[2*bin];data_[2*bin+1]=-input[2*bin+1];
    }
}
void OouraBackend::execute() noexcept {
    const bool inverse=request_.transform==FourierBackendTransform::RealInverse;
    aubio_ooura_rdft(int(request_.samples),inverse?-1:1,data_.get(),indices_.get(),tables_.get());
    if(!inverse)return;
    const float scale=float(2./double(request_.samples));
    for(std::size_t i=0;i<request_.samples;++i)data_[i]*=scale;
}
void OouraBackend::publish(std::span<float> output) const noexcept {
    if(request_.transform==FourierBackendTransform::RealInverse){std::copy_n(data_.get(),output.size(),output.begin());return;}
    output[0]=data_[0];output[1]=0;output[request_.samples]=data_[1];output[request_.samples+1]=0;
    for(std::size_t bin=1;bin<request_.samples/2;++bin){
        output[2*bin]=data_[2*bin];output[2*bin+1]=-data_[2*bin+1];
    }
}
bool OouraBackend::process(std::span<const float> input,std::span<float> output) noexcept {
    if(!data_ || !fourier_backend_call_valid(request_,input,output))return false;
    load(input);execute();
    if(!std::all_of(data_.get(),data_.get()+request_.samples,[](float value){return std::isfinite(value);}))return false;
    publish(output);return true;
}
FourierBackendResources OouraBackend::resources() const noexcept {
    FourierBackendResources result;storage(request_,result);return result;
}
std::unique_ptr<FourierBackend> OouraBackend::clone() const {
    auto result=std::make_unique<OouraBackend>(request_);
    return result->prepare()?std::unique_ptr<FourierBackend>(std::move(result)):nullptr;
}
class Factory final:public FourierBackendFactory {
public:
    const char* id() const noexcept override {return "aubio_ooura_fft8g_float32";}
    bool query(FourierBackendRequest request,FourierBackendResources& output) const noexcept override {return storage(request,output);}
    std::unique_ptr<FourierBackend> prepare(FourierBackendRequest request) const override {
        if(!admitted(request))return {};
        auto result=std::make_unique<OouraBackend>(request);
        return result->prepare()?std::unique_ptr<FourierBackend>(std::move(result)):nullptr;
    }
};
}
const dsp_kernels::FourierBackendFactory& ooura_backend_factory() noexcept {static const Factory factory;return factory;}
}
