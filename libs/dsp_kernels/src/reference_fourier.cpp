#include <dsp_kernels/detail/prepared_reference_fourier.h>
#include <dsp_kernels/prepared_storage.h>
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define POCKETFFT_NO_MULTITHREADING
#include "../vendor/pocketfft/pocketfft_hdronly.h"
#endif
#include <algorithm>
#include <cmath>
#include <utility>

namespace dsp_kernels::detail {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
struct ReferenceFourierPlan {
    pocketfft::detail::rfftp<double> transform;
    explicit ReferenceFourierPlan(std::size_t size):transform(size){}
};
namespace {
bool finite(std::span<const double> input) noexcept {
    return std::all_of(input.begin(),input.end(),[](double x){return std::isfinite(x);});
}
}
bool PreparedReferenceFourier::storage_bytes(std::size_t size,std::size_t& bytes) noexcept {
    // Twiddles, factors, two execution arrays, shared plan and all cold sin/cos
    // construction arrays coexist within this conservative bound. Factorization
    // and prime-size execution are the pinned real FFTPACK implementation.
    if(!size || size>65536)return false;
    std::size_t required=4096;
    if(!prepared_storage::add_array(required,size,64*sizeof(double)))return false;
    bytes=required;return true;
}
bool PreparedReferenceFourier::prepare(std::size_t size) {
    std::size_t bound=0;if(!storage_bytes(size,bound))return false;
    PreparedReferenceFourier next;next.plan_=std::make_shared<const ReferenceFourierPlan>(size);
    next.packed_.resize(size);next.scratch_.resize(size);
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedReferenceFourier::forward(std::span<const double> input,std::span<double> output) noexcept {
    const auto size=packed_.size();
    if(!plan_ || input.size()!=size || output.size()!=2*(size/2+1) || !finite(input))return false;
    std::copy(input.begin(),input.end(),packed_.begin());plan_->transform.exec(packed_.data(),1.,true,scratch_.data());
    output[0]=packed_[0];output[1]=0;
    for(std::size_t k=1;k<=size/2;++k){output[2*k]=packed_[2*k-1];output[2*k+1]=2*k<size?packed_[2*k]:0.;}
    return finite(output);
}
bool PreparedReferenceFourier::inverse(std::span<const double> input,std::span<double> output) noexcept {
    const auto size=packed_.size();
    if(!plan_ || input.size()!=2*(size/2+1) || output.size()!=size || !finite(input))return false;
    packed_[0]=input[0];
    for(std::size_t k=1;k<=size/2;++k){packed_[2*k-1]=input[2*k];if(2*k<size)packed_[2*k]=input[2*k+1];}
    plan_->transform.exec(packed_.data(),1./double(size),false,scratch_.data());
    std::copy(packed_.begin(),packed_.end(),output.begin());return finite(output);
}
std::size_t PreparedReferenceFourier::workspace_bytes() const noexcept {
    return (packed_.capacity()+scratch_.capacity())*sizeof(double)+(plan_?sizeof(ReferenceFourierPlan)+plan_->transform.prepared_storage_bytes():0);
}
#else
// The portable SDK deliberately disables exceptions. Its existing Float32
// engine remains available; asking for this desktop-only plan is rejected
// through the same explicit checked preparation contract, before processing.
bool PreparedReferenceFourier::storage_bytes(std::size_t,std::size_t&) noexcept {return false;}
bool PreparedReferenceFourier::prepare(std::size_t) {return false;}
bool PreparedReferenceFourier::forward(std::span<const double>,std::span<double>) noexcept {return false;}
bool PreparedReferenceFourier::inverse(std::span<const double>,std::span<double>) noexcept {return false;}
std::size_t PreparedReferenceFourier::workspace_bytes() const noexcept {return 0;}
#endif
}
