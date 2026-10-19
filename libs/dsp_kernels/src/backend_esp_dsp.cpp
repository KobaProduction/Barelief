#include "backend.h"
#include "dsp_kernels/dsp.h"
#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
#include "fft_profile.h"
#include "fft_ae32.h"
namespace dsp_kernels {
Backend compiled_backend() noexcept { return Backend::EspDsp; }
namespace detail {
static bool transform_impl(float *scratch, std::size_t n, bool radix4, bool real_kernel) {
    // Let the SDK select ANSI/ae32/aes3/arp4 for the configured SoC.
    DSP_FFT_BEGIN(Kernel);
#if defined(DSP_FFT_AE32)
    int result;
    if(real_kernel && radix4) {
#if defined(DSP_FFT_AES3)
        result=dsp_rfft_fft4r_aes3_(scratch,n,dsps_fft4r_w_table_fc32,dsps_fft4r_w_table_size);
#else
        result=dsp_rfft_fft4r_ae32_(scratch,n,dsps_fft4r_w_table_fc32,dsps_fft4r_w_table_size);
#endif
    }
#if defined(DSP_FFT_EXTRA)
    else if(real_kernel && !radix4) {
#if defined(DSP_FFT_AES3)
        result=dsp_rfft_fft2r_aes3_(scratch,n,dsps_fft_w_table_fc32);
#else
        result=dsp_rfft_fft2r_ae32_(scratch,n,dsps_fft_w_table_fc32);
#endif
    }
#endif
    else result=radix4 ? dsps_fft4r_fc32(scratch,n) : dsps_fft2r_fc32(scratch,n);
#else
    (void)real_kernel;
    const auto result = radix4 ? dsps_fft4r_fc32(scratch,n) : dsps_fft2r_fc32(scratch,n);
#endif
    DSP_FFT_END(Kernel);
    if (result != ESP_OK) return false;
    DSP_FFT_BEGIN(Permute);
    const auto permutation = radix4 ? dsps_bit_rev4r_fc32(scratch, n) : dsps_bit_rev2r_fc32(scratch, n);
    DSP_FFT_END(Permute);
    return permutation == ESP_OK;
}
bool transform(float* scratch,std::size_t n,bool radix4) { return transform_impl(scratch,n,radix4,false); }
bool transform_rfft_quarter(float* scratch,std::size_t n,bool optimized) { return transform_impl(scratch,n,true,optimized); }
bool transform_rfft_half(float* scratch,std::size_t n,bool radix4,bool optimized) { return transform_impl(scratch,n,radix4,optimized); }
}
}
