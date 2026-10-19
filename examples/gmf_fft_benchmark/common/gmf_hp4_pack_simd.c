/* SPDX-License-Identifier: Apache-2.0
 * Same two-pass BFP policy as gmf_hp4_pack.c, four wide SIMD pairs per batch.
 * Fixed-size stack scratch only. No allocation, no spectrum truncation. */
#include "gmf_radix4_simd.h"
#include "gmf_radix4_profile.h"
#include "gmf_complex_copy.h"
#include <string.h>
static void pair(const int16_t* data,const int16_t* w,int m,int k,int inverse,int32_t* t,int bias) {
    const int32_t ar=data[2*k],ai=data[2*k+1],br=data[2*(m-k)],bi=data[2*(m-k)+1];
    const int32_t sr=ar+br,si=ai-bi,dr=ar-br,di=ai+bi,wr=w[2*(k-1)],wi=(inverse?-1:1)*w[2*(k-1)+1];
    const int32_t tr=(dr*wr-di*wi+bias)>>14,ti=(dr*wi+di*wr+bias)>>14;
    t[0]=sr+tr;t[1]=si+ti;t[2]=sr-tr;t[3]=ti-si;
}
static void pairs(const int16_t* data,const int16_t* w,int m,int k,int inverse,int32_t* t,int bias_quarter,int32_t* extrema) {
#if !defined(GMF_R4_S3_SIMD)
    _Alignas(16) int16_t a[8],b[8];
    gmf_copy_complex(a,data+2*k);gmf_copy_complex(a+2,data+2*k+2);
    gmf_copy_complex(a+4,data+2*k+4);gmf_copy_complex(a+6,data+2*k+6);
    for(int j=0;j<4;++j)gmf_copy_complex(b+2*j,data+2*(m-k-j));
#endif
    GMF_R4_PROFILE_BEGIN(start);
#if defined(GMF_R4_S3_SIMD)
    if(extrema){
        if(inverse)gmf_r4_s3_pairs_range_inverse(data+2*k,data+2*(m-k-3),w+2*(k-1),t,bias_quarter,extrema);
        else gmf_r4_s3_pairs_range_forward(data+2*k,data+2*(m-k-3),w+2*(k-1),t,bias_quarter,extrema);
    }
    else if(inverse)gmf_r4_s3_pairs_direct_inverse(data+2*k,data+2*(m-k-3),w+2*(k-1),t,bias_quarter);
    else gmf_r4_s3_pairs_direct_forward(data+2*k,data+2*(m-k-3),w+2*(k-1),t,bias_quarter);
#else
    (void)extrema;
    gmf_r4_real_pairs_model(a,b,w+2*(k-1),t,inverse,bias_quarter);
#endif
    GMF_R4_PROFILE_END(start,GMF_R4_PAIRS);
}
static void range(int32_t v,int32_t* lo,int32_t* hi){if(v<*lo)*lo=v;if(v>*hi)*hi=v;}
static int16_t quantize(int32_t v,int s){return (int16_t)((v+(s?1<<(s-1):0))>>s);}
#if defined(GMF_R4_S3_SIMD)
static void publish_pairs(int16_t* data,const int16_t* w,int m,int k,int inverse,int bias_quarter,int shift,int rounding) {
    GMF_R4_PROFILE_BEGIN(start);
    if(inverse)gmf_r4_s3_pairs_publish_inverse(data+2*k,data+2*(m-k-3),w+2*(k-1),bias_quarter,shift,rounding);
    else gmf_r4_s3_pairs_publish_forward(data+2*k,data+2*(m-k-3),w+2*(k-1),bias_quarter,shift,rounding);
    GMF_R4_PROFILE_END(start,GMF_R4_PAIRS);
}
#endif
int32_t gmf_hp4_pack_simd(int16_t* data,const int16_t* w,int32_t m,int inverse) {
    GMF_R4_PROFILE_BEGIN(start);
    const int base=inverse?0:1;const int32_t a=data[0],b=data[1];
    const int32_t edge[2]={(a+b)*(inverse?1:2),(a-b)*(inverse?1:2)};
    const int k=m/2;const int32_t ar=data[2*k],ai=data[2*k+1],wr=w[2*(k-1)],wi=(inverse?-1:1)*w[2*(k-1)+1];
    const int32_t mid0=ar+ar+((-(ai+ai)*wi+8192)>>14);
    const int32_t mid1=((ai+ai)*wr+8192)>>14;
    int32_t bounds[2]={0,0};range(edge[0],bounds,bounds+1);range(edge[1],bounds,bounds+1);
    range(mid0,bounds,bounds+1);range(mid1,bounds,bounds+1);
    _Alignas(16) int32_t t[16];int j=1;
    for(;j+3<m/2;j+=4){pairs(data,w,m,j,inverse,t,2048,bounds);
#if !defined(GMF_R4_S3_SIMD)
        for(int z=0;z<16;++z)range(t[z],bounds,bounds+1);
#endif
    }
    for(;j<m/2;++j){pair(data,w,m,j,inverse,t,8192);for(int z=0;z<4;++z)range(t[z],bounds,bounds+1);}
    int s=base;
    for(;;++s){int32_t bias=s?1<<(s-1):0;if(((bounds[0]+bias)>>s)>=-32768 && ((bounds[1]+bias)>>s)<=32767)break;}
    data[0]=quantize(edge[0],s);data[1]=quantize(edge[1],s);
    if(!inverse){data[2*m]=data[1];data[2*m+1]=0;}
    j=1;
    for(;j+3<m/2;j+=4){
#if defined(GMF_R4_S3_SIMD)
        publish_pairs(data,w,m,j,inverse,2048,s,s?1<<(s-1):0);
#else
        pairs(data,w,m,j,inverse,t,2048,0);
        for(int z=0;z<4;++z){
        data[2*(j+z)]=quantize(t[z],s);data[2*(j+z)+1]=quantize(t[4+z],s);
        data[2*(m-j-z)]=quantize(t[8+z],s);data[2*(m-j-z)+1]=quantize(t[12+z],s);}
#endif
    }
    for(;j<m/2;++j){pair(data,w,m,j,inverse,t,8192);data[2*j]=quantize(t[0],s);data[2*j+1]=quantize(t[1],s);
        data[2*(m-j)]=quantize(t[2],s);data[2*(m-j)+1]=quantize(t[3],s);}
    data[m]=quantize(mid0,s);data[m+1]=quantize(mid1,s);
    GMF_R4_PROFILE_END(start,GMF_R4_REAL);return s-base;
}
static int16_t sat(int32_t v){return v>32767?32767:v< -32768?-32768:(int16_t)v;}
void gmf_q15_pack_simd(int16_t* data,const int16_t* w,int32_t m,int inverse) {
    GMF_R4_PROFILE_BEGIN(start);
    const int s=inverse?0:1;const int32_t a=data[0],b=data[1];
    data[0]=sat(a+b);data[1]=sat(a-b);if(!inverse){data[2*m]=data[1];data[2*m+1]=0;}
    _Alignas(16) int32_t t[16];int j=1;
    for(;j+3<m/2;j+=4){
#if defined(GMF_R4_S3_SIMD)
        publish_pairs(data,w,m,j,inverse,0,s,0);
#else
        pairs(data,w,m,j,inverse,t,0,0);
        for(int z=0;z<4;++z){
        data[2*(j+z)]=sat(t[z]>>s);data[2*(j+z)+1]=sat(t[4+z]>>s);
        data[2*(m-j-z)]=sat(t[8+z]>>s);data[2*(m-j-z)+1]=sat(t[12+z]>>s);}
#endif
    }
    for(;j<m/2;++j){pair(data,w,m,j,inverse,t,0);data[2*j]=sat(t[0]>>s);data[2*j+1]=sat(t[1]>>s);
        data[2*(m-j)]=sat(t[2]>>s);data[2*(m-j)+1]=sat(t[3]>>s);}
    const int k=m/2;const int32_t ar=data[2*k],ai=data[2*k+1],wr=w[2*(k-1)],wi=(inverse?-1:1)*w[2*(k-1)+1];
    data[m]=sat((ar+ar+((-(ai+ai)*wi)>>14))>>s);data[m+1]=sat((((ai+ai)*wr)>>14)>>s);
    GMF_R4_PROFILE_END(start,GMF_R4_REAL);
}
