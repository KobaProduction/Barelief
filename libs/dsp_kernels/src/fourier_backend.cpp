#include <dsp_kernels/fourier_backend.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace dsp_kernels {
namespace {
bool real_transform(FourierBackendTransform kind) noexcept {
    return kind==FourierBackendTransform::RealForward || kind==FourierBackendTransform::RealInverse;
}
bool disjoint(std::span<const float> input,std::span<float> output) noexcept {
    const std::less<const float*> before;
    return !before(input.data(),output.data()+output.size()) || !before(output.data(),input.data()+input.size());
}
bool admitted(FourierBackendResources actual,FourierBackendResources bound) noexcept {
    return actual.cold_peak_bytes>=actual.owned_bytes && actual.owned_bytes<=bound.owned_bytes && actual.cold_peak_bytes<=bound.cold_peak_bytes &&
        actual.shared_bytes<=bound.shared_bytes && actual.hot_stack_bytes<=bound.hot_stack_bytes;
}
[[noreturn]] void clone_failed() {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    throw std::bad_alloc();
#else
    std::terminate();
#endif
}
}
bool fourier_backend_sizes(FourierBackendRequest request,std::size_t& input,std::size_t& output) noexcept {
    const auto n=request.samples;
    if(!n || n>(std::numeric_limits<std::size_t>::max()-2)/2 ||
       unsigned(request.transform)>unsigned(FourierBackendTransform::ComplexInverse))return false;
    if(real_transform(request.transform) && (n<2 || n%2))return false;
    input=real_transform(request.transform)?n:2*n;output=real_transform(request.transform)?n+2:2*n;
    if(request.transform==FourierBackendTransform::RealInverse)std::swap(input,output);
    return true;
}
bool fourier_backend_call_valid(FourierBackendRequest request,std::span<const float> input,std::span<float> output) noexcept {
    std::size_t in=0,out=0;
    if(!fourier_backend_sizes(request,in,out) || input.size()!=in || output.size()!=out || !disjoint(input,output))return false;
    if(!std::all_of(input.begin(),input.end(),[](float v){return std::isfinite(v);}))return false;
    return request.transform!=FourierBackendTransform::RealInverse || (input[1]==0 && input[request.samples+1]==0);
}
PreparedFourierBackend::PreparedFourierBackend(const PreparedFourierBackend& other) {
    if(other.backend_){backend_=other.backend_->clone();if(!backend_)clone_failed();}
}
PreparedFourierBackend& PreparedFourierBackend::operator=(const PreparedFourierBackend& other) {
    if(this!=&other){PreparedFourierBackend next(other);*this=std::move(next);}return *this;
}
bool PreparedFourierBackend::query(FourierBackendRequest request,const FourierBackendFactory& factory,FourierBackendResources& output) noexcept {
    std::size_t in=0,out=0;FourierBackendResources resources;
    if(!fourier_backend_sizes(request,in,out) || !factory.query(request,resources) || resources.cold_peak_bytes<resources.owned_bytes)return false;
    output=resources;return true;
}
bool PreparedFourierBackend::prepare(FourierBackendRequest request,const FourierBackendFactory& factory) {
    backend_.reset();FourierBackendResources bound;
    if(!query(request,factory,bound))return false;
    auto candidate=factory.prepare(request);
    if(!candidate || !admitted(candidate->resources(),bound))return false;
    backend_=std::move(candidate);return true;
}
bool PreparedFourierBackend::process(std::span<const float> input,std::span<float> output) noexcept {
    return backend_ && backend_->process(input,output);
}
FourierBackendResources PreparedFourierBackend::resources() const noexcept {
    return backend_?backend_->resources():FourierBackendResources{};
}
}
