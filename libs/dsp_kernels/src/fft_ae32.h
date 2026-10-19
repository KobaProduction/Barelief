#pragma once
#if defined(DSP_FFT_AE32)
#include <cstddef>
extern "C" {
bool dsp_fft_input_max_ae32(const float*, std::size_t, float*) noexcept;
bool dsp_rfft_copy_max_ae32(const float*, float*, std::size_t, float*) noexcept;
bool dsp_fft_all_finite_ae32(const float*, std::size_t) noexcept;
void dsp_rfft_split_ae32(float*, const float*, std::size_t) noexcept;
void dsp_rfft_mixed_ae32(float*, const float*, const float*, std::size_t) noexcept;
void dsp_rfft_pack_ae32(const float*, float*, std::size_t) noexcept;
bool dsp_rfft_pack_max_ae32(const float*, float*, std::size_t, float*) noexcept;
void dsp_rfft_fused_pairs_ae32(float*, const float*, const float*, std::size_t) noexcept;
int dsp_rfft_fft4r_ae32_(float*, int, float*, int) noexcept;
int dsp_rfft_fft2r_ae32_(float*, int, float*) noexcept;
int dsp_rfft_fft4r_aes3_(float*, int, float*, int) noexcept;
int dsp_rfft_fft2r_aes3_(float*, int, float*) noexcept;
}
#endif
