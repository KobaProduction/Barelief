/* SPDX-License-Identifier: Apache-2.0
 * Wide Hermitian pack/unpack with an explicitly tracked BFP shift.
 * Two passes avoid an extra int32 scratch buffer: first determine exact output
 * range, then recompute disjoint conjugate pairs and publish rounded int16.
 * No saturation is used to conceal overflow or to discard a spectral component.
 */
#include "gmf_optimized.h"
#include <stdint.h>
#if defined(__GNUC__)
#define GMF_INLINE static inline __attribute__((always_inline))
#else
#define GMF_INLINE static inline
#endif
GMF_INLINE void pair(const int16_t* data,const int16_t* w,int m,int k,int inverse,int32_t t[4]) {
    const int32_t ar=data[2*k],ai=data[2*k+1],br=data[2*(m-k)],bi=data[2*(m-k)+1];
    const int32_t sr=ar+br,si=ai-bi,dr=ar-br,di=ai+bi;
    const int32_t wr=w[2*(k-1)],wi=(inverse?-1:1)*w[2*(k-1)+1];
    // |dr|,|di|<=65536 and Q14 twiddle L1<=23172: int32 is exact.
    const int32_t tr=(dr*wr-di*wi+8192)>>14,ti=(dr*wi+di*wr+8192)>>14;
    t[0]=sr+tr;t[1]=si+ti;t[2]=sr-tr;t[3]=ti-si;
}
GMF_INLINE void middle(const int16_t* data,const int16_t* w,int m,int inverse,int32_t t[2]) {
    const int k=m/2;const int32_t ar=data[2*k],ai=data[2*k+1];
    const int32_t wr=w[2*(k-1)],wi=(inverse?-1:1)*w[2*(k-1)+1];
    const int32_t tr=(-(ai+ai)*wi+8192)>>14,ti=((ai+ai)*wr+8192)>>14;
    t[0]=ar+ar+tr;t[1]=ti;
}
GMF_INLINE void range(int32_t v,int32_t* lo,int32_t* hi) {if(v<*lo)*lo=v;if(v>*hi)*hi=v;}
GMF_INLINE int16_t quantize(int32_t value,int shift) {
    return (int16_t)((value+(shift?1<<(shift-1):0))>>shift);
}
int32_t gmf_opt_hp4_pack(int16_t* data,const int16_t* w,int32_t m,int inverse) {
    const int base=inverse?0:1;
    const int32_t a=data[0],b=data[1];
    const int32_t edge[2]={(a+b)*(inverse?1:2),(a-b)*(inverse?1:2)};
    int32_t lo=0,hi=0;
    range(edge[0],&lo,&hi);range(edge[1],&lo,&hi);
    for(int k=1;k<m/2;++k) {int32_t t[4];pair(data,w,m,k,inverse,t);for(int j=0;j<4;++j)range(t[j],&lo,&hi);}
    int32_t mid[2];middle(data,w,m,inverse,mid);range(mid[0],&lo,&hi);range(mid[1],&lo,&hi);
    int shift=base;
    for(;;++shift) {
        const int32_t bias=shift?1<<(shift-1):0;
        if(((lo+bias)>>shift)>=-32768 && ((hi+bias)>>shift)<=32767)break;
    }
    data[0]=quantize(edge[0],shift);data[1]=quantize(edge[1],shift);
    if(!inverse){data[2*m]=data[1];data[2*m+1]=0;}
    for(int k=1;k<m/2;++k) {
        int32_t t[4];pair(data,w,m,k,inverse,t);
        data[2*k]=quantize(t[0],shift);data[2*k+1]=quantize(t[1],shift);
        data[2*(m-k)]=quantize(t[2],shift);data[2*(m-k)+1]=quantize(t[3],shift);
    }
    data[m]=quantize(mid[0],shift);data[m+1]=quantize(mid[1],shift);
    return shift-base;
}
