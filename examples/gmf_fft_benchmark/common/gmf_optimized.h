#pragma once
#include "esp_gmf_fft.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_gmf_fft_err_t gmf_optimized_init(size_t n, esp_gmf_fft_handle_t* handle);
esp_gmf_fft_err_t gmf_optimized_forward(esp_gmf_fft_handle_t handle, int16_t* data, int radix4);
esp_gmf_fft_err_t gmf_optimized_inverse(esp_gmf_fft_handle_t handle, int16_t* data, int radix4);
esp_gmf_fft_err_t gmf_optimized_forward_hp(esp_gmf_fft_handle_t handle, int16_t* data, int radix4);
esp_gmf_fft_err_t gmf_optimized_inverse_hp(esp_gmf_fft_handle_t handle, int16_t* data, int radix4);
int32_t gmf_opt_fft_bf_s16_hp(int16_t*, int16_t*, int32_t, int32_t);
int32_t gmf_opt_ifft_bf_s16_hp(int16_t*, int16_t*, int32_t, int32_t);
void gmf_opt_hp_fftr(int16_t*, int16_t*, int32_t, int32_t);
void gmf_opt_hp_ffti(int16_t*, int16_t*, int32_t, int32_t);
int32_t gmf_opt_radix4_hp_s16(int16_t*, const int16_t*, int32_t, int32_t, int);
int32_t gmf_opt_hp4_pack(int16_t*, const int16_t*, int32_t, int);
void gmf_opt_bit_reverse_s16(int16_t*, int32_t, int32_t);
void gmf_opt_fft_bf_s16(int16_t*, int16_t*, int32_t, int32_t, int32_t);
void gmf_opt_ifft_bf_s16(int16_t*, int16_t*, int32_t, int32_t, int32_t);
void gmf_opt_fftr_s16(int16_t*, int16_t*, int32_t, int32_t);
void gmf_opt_ffti_s16(int16_t*, int16_t*, int32_t, int32_t);
void gmf_opt_radix4_s16(int16_t*, const int16_t*, int32_t, int32_t, int);
int32_t gmf_opt_product_q16(int32_t, int32_t, int32_t, int32_t);
#if defined(GMF_TEST_MATH)
int32_t gmf_test_product_round(int32_t, int32_t, int32_t, int32_t, int);
int gmf_test_stage_shift_4(const int16_t*,int);
void gmf_test_multiply_pair(int32_t,int32_t,int32_t,int32_t,int16_t*,int,int,int);
#endif
#ifdef __cplusplus
}
#endif
