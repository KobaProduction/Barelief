#include "rfft_split.h"
#include "fft_ae32.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <bit>
#include <cstdint>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define DSP_RFFT_SSE2 1
#else
#define DSP_RFFT_SSE2 0
#endif

namespace dsp_kernels::detail {
namespace {
struct MixedRotations {float a,b,r,i,l,q,p,z;};
MixedRotations mixed_rotations(const float* data,const float* w,std::size_t m,std::size_t h,std::size_t k,std::size_t j) noexcept {
        const float a=.5f*(data[2*k]+data[2*j]);
        const float b=.5f*(data[2*k+1]-data[2*j+1]);
        const float c=(data[2*k+1]+data[2*j+1]);
        const float d=(data[2*j]-data[2*k]);
        const float u=(data[m+2*k]+data[m+2*j]);
        const float v=(data[m+2*k+1]-data[m+2*j+1]);
        const float s=(data[m+2*k+1]+data[m+2*j+1]);
        const float t=(data[m+2*j]-data[m+2*k]);
        const float r=w[4*k]*u-w[4*k+1]*v, i=w[4*k]*v+w[4*k+1]*u;
        const float l=w[2*k]*c-w[2*k+1]*d, q=w[2*k]*d+w[2*k+1]*c;
        const auto index=3*k<=h ? 3*k : m-3*k;
        const float wr=3*k<=h ? w[2*index] : -w[2*index], wi=w[2*index+1];
        const float p=wr*s-wi*t, z=wr*t+wi*s;
    return {a,b,r,i,l,q,p,z};
}
void publish_mixed_rotations(float* out,std::size_t m,std::size_t k,std::size_t j,const MixedRotations& rotation) noexcept {
    const auto [a,b,r,i,l,q,p,z]=rotation;
        const float rp=a+r, rm=a-r, ip=b+i, im=i-b;
        const float p1=l+p, q1=q+z, p2=z-q, q2=p-l;
        out[2*k]=rp+p1; out[2*k+1]=ip+q1;
        out[2*(m-k)]=rp-p1; out[2*(m-k)+1]=q1-ip;
        out[2*j]=rm+p2; out[2*j+1]=im+q2;
        out[2*(m-j)]=rm-p2; out[2*(m-j)+1]=q2-im;
}
}
namespace {
void mixed_midpoint(float* out,const float* data,const float* w,std::size_t h,std::size_t m) noexcept {
    const auto k=h/2;
    const float re=data[2*k], oi=data[m+2*k];
    const float x=(2.f*w[2*k])*(data[2*k+1]-data[m+2*k+1]);
    const float y=(2.f*w[2*k])*(data[2*k+1]+data[m+2*k+1]);
    out[2*k]=re+x; out[2*k+1]=-oi-y;
    out[2*(m-k)]=re-x; out[2*(m-k)+1]=oi-y;
}
}
void rfft_mixed_split(float* out, const float* data, const float* w, std::size_t h) noexcept {
    const auto m = 2*h, n = 4*h;
    const float zr = data[0]+data[m], zi = data[1]+data[m+1];
    out[0]=zr+zi; out[1]=0; out[n]=zr-zi; out[n+1]=0;
    out[m]=data[0]-data[m]; out[m+1]=data[m+1]-data[1];
#if defined(DSP_FFT_AE32)
    dsp_rfft_fused_pairs_ae32(out,data,w,h);
#else
    // For j=h-k, group the four final bins. The rotations reduce to
    // W_N^(2k)*(U+iV), W_N^k*(C+iD), W_N^(3k)*(S+iT).
    // This replaces two mixed butterflies plus two real split pairs.
    for(std::size_t k=1;k<h/2;++k) {
        const auto j=h-k;
        const auto rotation=mixed_rotations(data,w,m,h,k,j);
        publish_mixed_rotations(out,m,k,j,rotation);

    }
#endif
    mixed_midpoint(out,data,w,h,m);
}
static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559);
bool fft_all_finite(const float* data, std::size_t n) noexcept {
#if defined(DSP_FFT_AE32)
    return dsp_fft_all_finite_ae32(data, n);
#else
    for (std::size_t i = 0; i < n; ++i)
        if ((std::bit_cast<std::uint32_t>(data[i]) & 0x7f800000u) == 0x7f800000u) return false;
    return true;
#endif
}
void fft_copy(const float* source, float* destination, std::size_t n) noexcept {
#if defined(__XTENSA__)
    // Four independent word loads/stores, preserving subnormals and signed zero.
    // Avoid the toolchain's generic memmove path for known-disjoint FFT storage.
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const float a = source[i], b = source[i+1], c = source[i+2], d = source[i+3];
        destination[i] = a; destination[i+1] = b; destination[i+2] = c; destination[i+3] = d;
    }
    for (; i < n; ++i) destination[i] = source[i];
#else
    std::copy_n(source, n, destination);
#endif
}
#if DSP_RFFT_SSE2
namespace {
bool simd_input_max(const float* data,std::size_t n,std::size_t& i,float& maximum) noexcept {
    auto largest = _mm_setzero_ps();
    const auto sign = _mm_set1_ps(-0.f);
    const auto infinity = _mm_set1_ps(std::numeric_limits<float>::infinity());
    auto bad = _mm_setzero_ps();
    for (; i + 4 <= n; i += 4) {
        const auto x = _mm_andnot_ps(sign, _mm_loadu_ps(data + i));
        bad = _mm_or_ps(bad, _mm_cmpnlt_ps(x, infinity));
        largest = _mm_max_ps(largest, x);
    }
    if (_mm_movemask_ps(bad)) return false;
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, largest);
    maximum = std::max({lanes[0], lanes[1], lanes[2], lanes[3]});
    return true;
}
}
#endif
bool fft_input_max(const float* data, std::size_t n, float& maximum) noexcept {
#if defined(DSP_FFT_AE32)
    return dsp_fft_input_max_ae32(data, n, &maximum);
#else
    std::size_t i = 0;
    maximum = 0;
#if DSP_RFFT_SSE2
    if(!simd_input_max(data,n,i,maximum))return false;
#endif
    // Positive IEEE Float32 bit ordering matches magnitude ordering, including
    // subnormals. Classify exponent bits without floating-point helper calls.
    auto largest_bits = std::bit_cast<std::uint32_t>(maximum);
    for (; i < n; ++i) {
        const auto bits = std::bit_cast<std::uint32_t>(data[i]) & 0x7fffffffu;
        if (bits >= 0x7f800000u) return false;
        largest_bits = std::max(largest_bits, bits);
    }
    maximum = std::bit_cast<float>(largest_bits);
    return true;
#endif
}
const char* rfft_split_implementation() noexcept {
#if defined(DSP_FFT_AES3)
    return "aes3-f32";
#elif defined(DSP_FFT_AE32)
    return "ae32-f32";
#elif DSP_RFFT_SSE2
    return "sse2-f32";
#else
    return "scalar-f32";
#endif
}
#if DSP_RFFT_SSE2
namespace {
void split_vector_pair(float* data,const float* twiddles,std::size_t packed,std::size_t k,__m128 half,__m128 conjugate,__m128 rotate) noexcept {
            const auto a = _mm_loadu_ps(data + 2 * k);
            const auto reversed = _mm_loadu_ps(data + 2 * (packed - k - 1));
            const auto b = _mm_xor_ps(_mm_shuffle_ps(reversed, reversed, _MM_SHUFFLE(1,0,3,2)), conjugate);
            const auto even = _mm_mul_ps(_mm_add_ps(a, b), half);
            const auto difference = _mm_sub_ps(a, b);
            const auto odd = _mm_mul_ps(_mm_xor_ps(
                _mm_shuffle_ps(difference, difference, _MM_SHUFFLE(2,3,0,1)), conjugate), half);
            const auto w = _mm_loadu_ps(twiddles + 2 * k);
            const auto wr = _mm_shuffle_ps(w, w, _MM_SHUFFLE(2,2,0,0));
            const auto wi = _mm_shuffle_ps(w, w, _MM_SHUFFLE(3,3,1,1));
            const auto swapped = _mm_xor_ps(_mm_shuffle_ps(odd, odd, _MM_SHUFFLE(2,3,0,1)), rotate);
            const auto product = _mm_add_ps(_mm_mul_ps(wr, odd), _mm_mul_ps(wi, swapped));
            _mm_storeu_ps(data + 2 * k, _mm_add_ps(even, product));
            const auto opposite = _mm_xor_ps(_mm_sub_ps(even, product), conjugate);
            _mm_storeu_ps(data + 2 * (packed - k - 1),
                _mm_shuffle_ps(opposite, opposite, _MM_SHUFFLE(1,0,3,2)));
}
void split_vector_pairs(float* data,const float* twiddles,std::size_t packed,std::size_t& k) noexcept {
        const auto half = _mm_set1_ps(.5f);
        const auto conjugate = _mm_setr_ps(0.f, -0.f, 0.f, -0.f);
        const auto rotate = _mm_setr_ps(-0.f, 0.f, -0.f, 0.f);
        // Two independent split pairs per register. Both sides are loaded
        // before either is stored; the self-conjugate midpoint is scalar.
        for (; k + 1 < packed / 2; k += 2) {
            split_vector_pair(data,twiddles,packed,k,half,conjugate,rotate);
        }
}
}
#endif
void rfft_split(float* data, const float* twiddles, std::size_t packed, bool scalar) noexcept {
#if defined(DSP_FFT_AE32)
    if (!scalar) { dsp_rfft_split_ae32(data, twiddles, packed); return; }
#endif
    std::size_t k = 1;
#if DSP_RFFT_SSE2
    if (!scalar) {
        split_vector_pairs(data,twiddles,packed,k);
    }
#else
    (void)scalar;
#endif
    for (; k <= packed / 2; ++k) {
        const auto opposite = packed - k;
        const float ar = data[2*k], ai = data[2*k+1];
        const float br = data[2*opposite], bi = data[2*opposite+1];
        const float er = .5f*(ar+br), ei = .5f*(ai-bi);
        const float ore = .5f*(ai+bi), oim = -.5f*(ar-br);
        const float wr = twiddles[2*k], wi = twiddles[2*k+1];
        data[2*k] = er + wr*ore - wi*oim;
        data[2*k+1] = ei + wr*oim + wi*ore;
        if (opposite != k) {
            data[2*opposite] = er - wr*ore + wi*oim;
            data[2*opposite+1] = -ei + wr*oim + wi*ore;
        }
    }
}
}
