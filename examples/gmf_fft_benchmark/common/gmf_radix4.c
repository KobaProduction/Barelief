/* SPDX-License-Identifier: Apache-2.0
 * Mixed radix-4/2 DIF over the same Q14 stage tables as pinned GMF.
 * Each radix-4 stage divides by four; an odd remaining stage divides by two.
 * Four-input products need more than the two-input int32 bound. Exact quotient
 * decomposition below avoids int64 without dropping a single product bit.
 * Quantization occurs once per radix-4 stage, so this
 * candidate is mathematically equivalent but NOT bit-identical to radix-2.
 */
#include "gmf_optimized.h"
#include <stdint.h>
#if defined(__GNUC__)
#define GMF_INLINE static inline __attribute__((always_inline))
#else
#define GMF_INLINE static inline
#endif
GMF_INLINE int16_t sat(int32_t v) { return v>32767?32767:v< -32768?-32768:(int16_t)v; }
GMF_INLINE int32_t product_q16(int32_t re,int32_t im,int32_t wr,int32_t wi) {
    // Most ordinary audio butterflies fit the two-input bound. The fallback
    // below is exact for the entire four-input range, so no gain is changed.
    if ((uint32_t)(re+65536)<=131072u && (uint32_t)(im+65536)<=131072u)
        return (re*wr+im*wi)>>16;
    /* |re|,|im|<=131072; generated Q14 twiddles have L1<=23172.
     * p=(re>>1)*wr+(im>>1)*wi fits int32 (<=1518600192).
     * re=2*(re>>1)+(re&1), also for negative two's-complement values.
     * Therefore floor((re*wr+im*wi)/65536) equals the expression below.
     * Its residual lies in [-23172,88706]; no intermediate can overflow.
     * Arithmetic right shift follows the same GCC target convention as GMF.
     */
    const int32_t p=(re>>1)*wr+(im>>1)*wi;
    const int32_t remainder=2*(p&32767)+(re&1)*wr+(im&1)*wi;
    return (p>>15)+(remainder>>16);
}
int32_t gmf_opt_product_q16(int32_t re,int32_t im,int32_t wr,int32_t wi) {
    return product_q16(re,im,wr,wi);
}
GMF_INLINE int32_t product_round(int32_t re,int32_t im,int32_t wr,int32_t wi,int shift) {
    const int32_t rounding=1<<(shift-1);
    if ((uint32_t)(re+65536)<=131072u && (uint32_t)(im+65536)<=131072u)
        return (re*wr+im*wi+rounding)>>shift;
    const int32_t p=(re>>1)*wr+(im>>1)*wi;
    const int32_t mask=(1<<(shift-1))-1;
    return (p>>(shift-1))+((2*(p&mask)+(re&1)*wr+(im&1)*wi+rounding)>>shift);
}
#if defined(GMF_TEST_MATH)
int32_t gmf_test_product_round(int32_t re,int32_t im,int32_t wr,int32_t wi,int shift) {
    return product_round(re,im,wr,wi,shift);
}
#endif
static int stage_shift(const int16_t* data,int points,int radix4) {
    uint32_t peak=0;
    for(int i=0;i<2*points;++i) {
        int32_t v=data[i];uint32_t a=(uint32_t)(v<0?-v:v);
        /*23169 requires shift3, already sufficient for every int16 peak. */
        if(radix4 && a>=23169u)return 3;
        if(a>peak)peak=a;
    }
    // A radix4 output component is a sum of four components, rotated by a
    // Q14 twiddle with L1<=23172. Include half-ULP rounding in the bound.
    const uint32_t bound=(radix4?4u:2u)*peak*23172u;
    int shift=0;
    while(((bound+(1u<<(13+shift)))>>(14+shift))>32767u) ++shift;
    return shift;
}
#if defined(GMF_TEST_MATH)
int gmf_test_stage_shift_4(const int16_t* data,int points){return stage_shift(data,points,1);}
#endif
/* Both output components share the exact narrow predicate and quotient parts.
 * The Q14 L1 bound and once-only bias/shift are unchanged. */
GMF_INLINE void multiply_pair(int32_t re,int32_t im,int32_t wr,int32_t wi,
                             int16_t* out,int inverse,int shift,int rounding) {
    const int32_t v=inverse?wi:-wi;
    int32_t r,i;
    if((uint32_t)(re+65536)<=131072u && (uint32_t)(im+65536)<=131072u){
        r=(re*wr-im*v+rounding)>>shift;
        i=(im*wr+re*v+rounding)>>shift;
    } else {
        const int32_t rh=re>>1,ih=im>>1,rl=re&1,il=im&1;
        const int32_t pr=rh*wr-ih*v,pi=ih*wr+rh*v;
        const int32_t mask=(1<<(shift-1))-1;
        r=(pr>>(shift-1))+((2*(pr&mask)+rl*wr-il*v+rounding)>>shift);
        i=(pi>>(shift-1))+((2*(pi&mask)+il*wr+rl*v+rounding)>>shift);
    }
    out[0]=sat(r);out[1]=sat(i);
}
#if defined(GMF_TEST_MATH)
void gmf_test_multiply_pair(int32_t re,int32_t im,int32_t wr,int32_t wi,int16_t* out,int inverse,int shift,int rounding){
    multiply_pair(re,im,wr,wi,out,inverse,shift,rounding);
}
#endif
GMF_INLINE void multiply_hp(int32_t re,int32_t im,int32_t wr,int32_t wi,
                        int16_t* out,int inverse,int shift) {
    multiply_pair(re,im,wr,wi,out,inverse,14+shift,1<<(13+shift));
}
int32_t gmf_opt_radix4_hp_s16(int16_t* data,const int16_t* table,
                            int32_t log2n,int32_t points,int inverse) {
    int offset=0,length=points,extra=0;
    while(length>=4) {
        const int q=length/4,half=length/2,shift=stage_shift(data,points,1);
        const int rounding=shift?1<<(shift-1):0;
        extra+=shift;
        const int16_t* w1=table+offset;const int16_t* w2=w1+length;
        for(int base=0;base<points;base+=length) for(int j=0;j<q;++j) {
            int16_t* a=data+2*(base+j);int16_t* b=a+2*q;int16_t* c=b+2*q;int16_t* d=c+2*q;
            const int32_t ur=(int32_t)a[0]+c[0],ui=(int32_t)a[1]+c[1];
            const int32_t vr=(int32_t)b[0]+d[0],vi=(int32_t)b[1]+d[1];
            const int32_t pr=(int32_t)a[0]-c[0],pi=(int32_t)a[1]-c[1];
            const int32_t qr=(int32_t)b[0]-d[0],qi=(int32_t)b[1]-d[1];
            const int s=inverse?-1:1;
            int k=3*j,sign=1;if(k>=half){k-=half;sign=-1;}
            a[0]=sat((ur+vr+rounding)>>shift);a[1]=sat((ui+vi+rounding)>>shift);
            multiply_hp(ur-vr,ui-vi,w2[2*j],w2[2*j+1],b,inverse,shift);
            multiply_hp(pr+s*qi,pi-s*qr,w1[2*j],w1[2*j+1],c,inverse,shift);
            multiply_hp(pr-s*qi,pi+s*qr,sign*w1[2*k],sign*w1[2*k+1],d,inverse,shift);
        }
        offset+=length+length/2;length/=4;log2n-=2;
    }
    if(log2n) {
        // Unity final radix2 has no rotation: its exact growth bound is2*peak.
        int32_t peak=0;
        for(int i=0;i<2*points;++i) {int32_t v=data[i];if(v<0)v=-v;if(v>peak)peak=v;}
        int shift=0;while(((2*peak+(shift?1<<(shift-1):0))>>shift)>32767)++shift;
        int rounding=shift?1<<(shift-1):0;extra+=shift;
        for(int i=0;i<points;i+=2) {
            int32_t ar=data[2*i],ai=data[2*i+1],br=data[2*i+2],bi=data[2*i+3];
            data[2*i]=sat((ar+br+rounding)>>shift);data[2*i+1]=sat((ai+bi+rounding)>>shift);
            data[2*i+2]=sat((ar-br+rounding)>>shift);data[2*i+3]=sat((ai-bi+rounding)>>shift);
        }
    }
    return extra;
}
GMF_INLINE void multiply(int32_t re, int32_t im, int32_t wr, int32_t wi,
                     int16_t* out, int inverse) {
    multiply_pair(re,im,wr,wi,out,inverse,16,0);
}
void gmf_opt_radix4_s16(int16_t* data, const int16_t* table,
                       int32_t log2n, int32_t points, int inverse) {
    int32_t offset=0, length=points;
    while(length>=4) {
        const int32_t q=length/4, half=length/2;
        const int16_t* w1=table+offset;
        const int16_t* w2=w1+length;
        for(int32_t base=0;base<points;base+=length) {
            for(int32_t j=0;j<q;++j) {
                int16_t* a=data+2*(base+j);
                int16_t* b=a+2*q;
                int16_t* c=b+2*q;
                int16_t* d=c+2*q;
                const int32_t ur=(int32_t)a[0]+c[0], ui=(int32_t)a[1]+c[1];
                const int32_t vr=(int32_t)b[0]+d[0], vi=(int32_t)b[1]+d[1];
                const int32_t pr=(int32_t)a[0]-c[0], pi=(int32_t)a[1]-c[1];
                const int32_t qr=(int32_t)b[0]-d[0], qi=(int32_t)b[1]-d[1];
                const int32_t s=inverse?-1:1;
                /* The table covers [0,pi); reconstruct W^(3j) by symmetry. */
                int32_t k=3*j, sign=1;
                if(k>=half) {k-=half;sign=-1;}
                a[0]=sat((ur+vr)>>2); a[1]=sat((ui+vi)>>2);
                multiply(ur-vr,ui-vi,w2[2*j],w2[2*j+1],b,inverse);
                multiply(pr+s*qi,pi-s*qr,w1[2*j],w1[2*j+1],c,inverse);
                multiply(pr-s*qi,pi+s*qr,sign*w1[2*k],sign*w1[2*k+1],d,inverse);
            }
        }
        offset+=length+length/2;
        length/=4;
        log2n-=2;
    }
    if(log2n) {
        /* Final unity radix-2 stage; fixed 1/2 gain. */
        for(int32_t i=0;i<points;i+=2) {
            int32_t ar=data[2*i], ai=data[2*i+1];
            int32_t br=data[2*i+2], bi=data[2*i+3];
            data[2*i]=sat((ar+br)>>1); data[2*i+1]=sat((ai+bi)>>1);
            data[2*i+2]=sat((ar-br)>>1); data[2*i+3]=sat((ai-bi)>>1);
        }
    }
}
