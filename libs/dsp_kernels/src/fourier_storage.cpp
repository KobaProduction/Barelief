#include <dsp_kernels/fourier_transform.h>
#include <dsp_kernels/detail/fourier_geometry.h>
#include <dsp_kernels/prepared_storage.h>
#include <bit>

namespace dsp_kernels {
using namespace prepared_storage;
namespace {
bool convolution_fft_storage(std::size_t capacity,std::size_t& bytes) noexcept {
    if(capacity>max_fft_samples)return detail::PreparedBlockedFourier::storage_bytes(capacity,bytes);
    return capacity<4 || FFT::storage_bytes(capacity,bytes);
}
bool transform_buffer_storage(std::size_t samples,std::size_t capacity,std::size_t& bytes) noexcept {
    if(!add_array(bytes,capacity,4*sizeof(float)) || !add_array(bytes,samples,2*sizeof(float)))return false;
    return capacity==samples || (add_array(bytes,samples,2*sizeof(float)) && add_array(bytes,capacity,2*sizeof(float)));
}
bool backend_storage(std::size_t n,const FourierBackendFactory& factory,std::size_t& output) noexcept {
    FourierBackendResources forward,inverse;std::size_t bytes=0;
    if(!PreparedFourierBackend::query({n,FourierBackendTransform::ComplexForward},factory,forward) ||
       !PreparedFourierBackend::query({n,FourierBackendTransform::ComplexInverse},factory,inverse))return false;
    if(!add_bytes(bytes,forward.cold_peak_bytes) || !add_bytes(bytes,inverse.cold_peak_bytes) || !add_array(bytes,n,4*sizeof(float)))return false;
    output=bytes;return true;
}
}
bool FFT::storage_bytes(std::size_t samples,std::size_t& bytes) noexcept {
    if(samples<4 || samples>max_fft_samples || !std::has_single_bit(samples))return false;
    return mul_bytes(samples,2*sizeof(float),bytes);
}
bool PreparedRfft::storage_bytes(std::size_t samples,bool radix4,std::size_t& output) noexcept {
    if(samples<8 || samples>max_fft_samples || !std::has_single_bit(samples))return false;
    const bool mixed=radix4 && ((samples/2)&0x55555555)==0;
    std::size_t bytes=0;
    if(!add_array(bytes,samples+2,sizeof(float)) || !add_array(bytes,samples/2+2,sizeof(float)) ||
       (mixed && !add_array(bytes,samples,sizeof(float))))return false;
    output=bytes;return true;
}
bool detail::PreparedBlockedFourier::storage_bytes(std::size_t samples,std::size_t& output) noexcept {
    if(samples<=max_fft_samples || !std::has_single_bit(samples) || samples>SIZE_MAX/4)return false;
    std::size_t bytes=0;
    if(!FFT::storage_bytes(max_fft_samples,bytes) || !add_array(bytes,max_fft_samples,4*sizeof(float)) ||
       !add_array(bytes,samples,sizeof(float)) || !add_array(bytes,samples/max_fft_samples,sizeof(std::size_t)))return false;
    output=bytes;return true;
}
bool PreparedFourierTransform::storage_bytes(std::size_t samples,std::size_t& output,const FourierBackendFactory* backend) noexcept {
    if(backend)return backend_storage(samples,*backend,output);
    const auto capacity=detail::fourier_convolution_size(samples);if(!capacity)return false;
    std::size_t bytes=0,nested=0;
    if(!transform_buffer_storage(samples,capacity,bytes) || !convolution_fft_storage(capacity,nested))return false;
    if(!add_bytes(bytes,nested))return false;output=bytes;return true;
}
} // namespace dsp_kernels
