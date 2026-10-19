/* Compile the unmodified pinned implementation and expose its private exponent
 * in the same translation unit. This avoids guessing HP amplitude from a fit. */
#include "fft_power2_q15.c"
#include "gmf_probe.h"
int gmf_probe_exponent(esp_gmf_fft_handle_t handle)
{
    return ((const gmf_fft_plan_t *)handle)->hp_spec_exp;
}
size_t gmf_probe_plan_bytes(void) { return sizeof(gmf_fft_plan_t); }

#include "gmf_optimized.h"
#include "gmf_radix4_simd.h"
#include "gmf_radix4_profile.h"
#include "gmf_permutation.h"
#if defined(GMF_R4_S3_SIMD) || defined(GMF_R4_ROUTE_MODEL)
#define GMF_R4_VECTOR_ROUTE 1
#endif
static void optimized_permutation(gmf_fft_plan_t* p,int16_t* data) {
    GMF_R4_PROFILE_BEGIN(start);
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if(p->log2_n<=10) fft_radix2_bit_reverse_s16(data,p->cpx_point,p->log2_n);
#if defined(GMF_S3_VECTOR_PERMUTATION)
    else if(!((uintptr_t)data&15u))gmf_s3_bit_reverse_s16(data,p->cpx_point,p->log2_n);
#endif
    else gmf_opt_bit_reverse_s16(data,p->cpx_point,p->log2_n);
#else
    gmf_opt_bit_reverse_s16(data,p->cpx_point,p->log2_n);
#endif
    GMF_R4_PROFILE_END(start,GMF_R4_PERMUTATION);
}
/* Own extension of the pinned plan to N=16384. The stock API and its limit
 * remain untouched; all tables have the same documented Q14 layout. */
esp_gmf_fft_err_t gmf_optimized_init(size_t n, esp_gmf_fft_handle_t* handle)
{
    if (!handle) return ESP_GMF_FFT_ERR_INVALID_ARG;
    *handle = NULL;
    if (n < 32 || n > 16384 || (n & (n-1))) return ESP_GMF_FFT_ERR_INVALID_ARG;
    if (n <= 8192) {
        esp_gmf_fft_cfg_t cfg = {(int16_t)n, ESP_GMF_FFT_TYPE_REAL_Q15};
        return esp_gmf_fft_init(&cfg, handle);
    }
    gmf_fft_plan_t* p = esp_gmf_fft_calloc_aligned(1, sizeof(*p), 16);
    if (!p) return ESP_GMF_FFT_ERR_NO_MEM;
    p->cpx_point = (uint16_t)(n/2);
    p->log2_n = (uint8_t)fft_log2_pow2(p->cpx_point);
    p->hp_spec_exp = FFT_HP_Q15_EXPONENT;
    p->twiddle_win = esp_gmf_fft_calloc_aligned(n, sizeof(int16_t), 16);
    p->twiddle_win2 = esp_gmf_fft_calloc_aligned(n/2, sizeof(int16_t), 16);
    if (!p->twiddle_win || !p->twiddle_win2) {fft_plan_free(p); return ESP_GMF_FFT_ERR_NO_MEM;}
    int16_t* w=p->twiddle_win;
    for (int count=(int)n/4; count; count/=2) {
        for (int j=0;j<count;++j) {
            double angle=M_PI*(double)j/count;
            *w++=fft_q14_from_double(cos(angle));
            *w++=fft_q14_from_double(sin(angle));
        }
    }
    for (int i=0;i<(int)n/4;++i) {
        double phase=-M_PI*((double)(i+1)/p->cpx_point+0.5);
        p->twiddle_win2[2*i]=fft_q14_from_double(cos(phase));
        p->twiddle_win2[2*i+1]=fft_q14_from_double(sin(phase));
    }
    *handle=p;
    return ESP_GMF_FFT_OK;
}
esp_gmf_fft_err_t gmf_optimized_forward(esp_gmf_fft_handle_t handle, int16_t* data, int radix4)
{
    if (!handle || !data) return ESP_GMF_FFT_ERR_INVALID_ARG;
#if defined(GMF_R4_S3_SIMD)
    if(radix4&&((uintptr_t)data&15u))return ESP_GMF_FFT_ERR_INVALID_ARG;
#endif
    gmf_fft_plan_t* p=handle;
    if (radix4) {
#if defined(GMF_R4_VECTOR_ROUTE)
        if(!((uintptr_t)data&15u))gmf_r4_simd(data,p->twiddle_win,p->log2_n,p->cpx_point,0,0);
        else
#endif
        gmf_opt_radix4_s16(data,p->twiddle_win,p->log2_n,p->cpx_point,0);
    }
    else {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        /* Retain upstream 128-bit PIE butterflies; optimize the permutation. */
        fft_radix2_fft_bf_s16(data,p->twiddle_win,15,p->log2_n,p->cpx_point);
#else
        gmf_opt_fft_bf_s16(data,p->twiddle_win,15,p->log2_n,p->cpx_point);
#endif
    }
    optimized_permutation(p,data);
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (!radix4) fft_radix2_fftr_s16(data,p->twiddle_win2,p->cpx_point,14);
    else
#endif
    {
#if defined(GMF_R4_VECTOR_ROUTE)
        if(radix4)gmf_q15_pack_simd(data,p->twiddle_win2,p->cpx_point,0);else
#endif
        gmf_opt_fftr_s16(data,p->twiddle_win2,p->cpx_point,14);
    }
    return ESP_GMF_FFT_OK;
}
esp_gmf_fft_err_t gmf_optimized_inverse(esp_gmf_fft_handle_t handle, int16_t* data, int radix4)
{
    if (!handle || !data) return ESP_GMF_FFT_ERR_INVALID_ARG;
#if defined(GMF_R4_S3_SIMD)
    if(radix4&&((uintptr_t)data&15u))return ESP_GMF_FFT_ERR_INVALID_ARG;
#endif
    gmf_fft_plan_t* p=handle;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (!radix4) fft_radix2_ffti_s16(data,p->twiddle_win2,p->cpx_point,14);
    else
#endif
    {
#if defined(GMF_R4_VECTOR_ROUTE)
        if(radix4)gmf_q15_pack_simd(data,p->twiddle_win2,p->cpx_point,1);else
#endif
        gmf_opt_ffti_s16(data,p->twiddle_win2,p->cpx_point,14);
    }
    if (radix4) {
#if defined(GMF_R4_VECTOR_ROUTE)
        if(!((uintptr_t)data&15u))gmf_r4_simd(data,p->twiddle_win,p->log2_n,p->cpx_point,1,0);
        else
#endif
        gmf_opt_radix4_s16(data,p->twiddle_win,p->log2_n,p->cpx_point,1);
    }
    else {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        fft_radix2_ifft_bf_s16(data,p->twiddle_win,15,p->log2_n,p->cpx_point);
#else
        gmf_opt_ifft_bf_s16(data,p->twiddle_win,15,p->log2_n,p->cpx_point);
#endif
    }
    optimized_permutation(p,data);
    return ESP_GMF_FFT_OK;
}
esp_gmf_fft_err_t gmf_optimized_forward_hp(esp_gmf_fft_handle_t handle,int16_t* data,int radix4) {
    if(!handle||!data)return ESP_GMF_FFT_ERR_INVALID_ARG;
#if defined(GMF_R4_S3_SIMD)
    if(radix4&&((uintptr_t)data&15u))return ESP_GMF_FFT_ERR_INVALID_ARG;
#endif
    gmf_fft_plan_t* p=handle;int shifts;
    if(radix4) {
#if defined(GMF_R4_VECTOR_ROUTE)
        if(!((uintptr_t)data&15u))shifts=gmf_r4_simd(data,p->twiddle_win,p->log2_n,p->cpx_point,0,1);
        else
#endif
        shifts=gmf_opt_radix4_hp_s16(data,p->twiddle_win,p->log2_n,p->cpx_point,0);
    }
    else {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        shifts=fft_radix2_fft_bf_s16_hp(data,p->twiddle_win,p->log2_n,p->cpx_point);
#else
        shifts=gmf_opt_fft_bf_s16_hp(data,p->twiddle_win,p->log2_n,p->cpx_point);
#endif
    }
    optimized_permutation(p,data);
    if(radix4) {
#if defined(GMF_R4_VECTOR_ROUTE)
        shifts+=gmf_hp4_pack_simd(data,p->twiddle_win2,p->cpx_point,0);
#else
        shifts+=gmf_opt_hp4_pack(data,p->twiddle_win2,p->cpx_point,0);
#endif
    }
    else {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        fft_hp_fftr(data,p->twiddle_win2,p->cpx_point,14);
#else
        gmf_opt_hp_fftr(data,p->twiddle_win2,p->cpx_point,14);
#endif
    }
    p->hp_spec_exp=-15+shifts;
    return ESP_GMF_FFT_OK;
}
esp_gmf_fft_err_t gmf_optimized_inverse_hp(esp_gmf_fft_handle_t handle,int16_t* data,int radix4) {
    if(!handle||!data)return ESP_GMF_FFT_ERR_INVALID_ARG;
#if defined(GMF_R4_S3_SIMD)
    if(radix4&&((uintptr_t)data&15u))return ESP_GMF_FFT_ERR_INVALID_ARG;
#endif
    gmf_fft_plan_t* p=handle;int shifts=0;
    if(radix4) {
#if defined(GMF_R4_VECTOR_ROUTE)
        shifts=gmf_hp4_pack_simd(data,p->twiddle_win2,p->cpx_point,1);
#else
        shifts=gmf_opt_hp4_pack(data,p->twiddle_win2,p->cpx_point,1);
#endif
    }
    else {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        fft_hp_ffti(data,p->twiddle_win2,p->cpx_point,14);
#else
        gmf_opt_hp_ffti(data,p->twiddle_win2,p->cpx_point,14);
#endif
    }
    if(radix4) {
#if defined(GMF_R4_VECTOR_ROUTE)
        if(!((uintptr_t)data&15u))shifts+=gmf_r4_simd(data,p->twiddle_win,p->log2_n,p->cpx_point,1,1);
        else
#endif
        shifts+=gmf_opt_radix4_hp_s16(data,p->twiddle_win,p->log2_n,p->cpx_point,1);
    }
    else {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        shifts+=fft_radix2_ifft_bf_s16_hp(data,p->twiddle_win,p->log2_n,p->cpx_point);
#else
        shifts+=gmf_opt_ifft_bf_s16_hp(data,p->twiddle_win,p->log2_n,p->cpx_point);
#endif
    }
    optimized_permutation(p,data);
    fft_hp_scale_to_q15(data,p->cpx_point*2,p->hp_spec_exp+shifts-p->log2_n-1);
    return ESP_GMF_FFT_OK;
}
