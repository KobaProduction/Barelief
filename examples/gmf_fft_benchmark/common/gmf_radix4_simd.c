/* SPDX-License-Identifier: Apache-2.0
 * True radix4 driver. Full stages use four-way SIMD batches. Small stages
 * gather butterflies across boxes into a fixed stack block, then scatter.
 * The host build substitutes an explicit PIE arithmetic semantic model.
 */
#include "gmf_radix4_simd.h"
#include "gmf_radix4_profile.h"
#include "gmf_complex_copy.h"
#include <string.h>
#if defined(GMF_R4_S3_SIMD)
_Static_assert(offsetof(gmf_r4_batch,twiddle)==16,"PIE batch ABI");
_Static_assert(offsetof(gmf_r4_batch,product_shift)==28,"PIE batch ABI");
_Static_assert(offsetof(gmf_r4_batch,bias_quarter)==32,"PIE batch ABI");
#endif
static int16_t sat(int32_t v){return v>32767?32767:v< -32768?-32768:(int16_t)v;}
static int32_t peak(const int16_t* data,int count) {
    GMF_R4_PROFILE_BEGIN(start);
#if defined(GMF_R4_S3_SIMD)
    int32_t m=gmf_r4_s3_peak(data,count);
#else
    int32_t m=0;for(int i=0;i<count;++i){int32_t v=data[i];if(v<0)v=-v;if(v>m)m=v;}
#endif
    GMF_R4_PROFILE_END(start,GMF_R4_PEAK);return m;
}
static int shift_hp(const int16_t* data,int points,int radix) {
    uint32_t bound=(uint32_t)peak(data,2*points)*radix;
    if(radix==4)bound*=23172u;
    int s=0;
    if(radix==4){while(((bound+(1u<<(13+s)))>>(14+s))>32767u)++s;}
    else{while(((bound+(s?1u<<(s-1):0))>>s)>32767u)++s;}
    return s;
}
static void batch(const gmf_r4_batch* c,int inverse) {
    GMF_R4_PROFILE_BEGIN(start);
#if defined(GMF_R4_S3_SIMD)
    if(inverse)gmf_r4_s3_batch_inverse(c);else gmf_r4_s3_batch_forward(c);
#else
    gmf_r4_batch_model(c,inverse);
#endif
    GMF_R4_PROFILE_END(start,GMF_R4_BATCH);
}
int32_t gmf_r4_simd(int16_t* data,const int16_t* table,int32_t log2n,
                    int32_t points,int inverse,int hp) {
    GMF_R4_PROFILE_BEGIN(start);
    int length=points,offset=0,extra=0;
    while(length>=4) {
        const int q=length/4,half=length/2,s=hp?shift_hp(data,points,4):2;
        const int16_t *w1=table+offset,*w2=w1+length;
        extra+=s;
        gmf_r4_batch c={{0},{0},14+s,hp?1<<(11+s):0};
        _Alignas(16) int16_t w3[8];
        if(q>=4) {
            for(int j=0;j<q;j+=4) {
                for(int t=0;t<4;++t){int k=3*(j+t),sign=1;if(k>=half){k-=half;sign=-1;}
                    w3[2*t]=sign*w1[2*k];w3[2*t+1]=sign*w1[2*k+1];}
                c.twiddle[0]=w1+2*j;c.twiddle[1]=w2+2*j;c.twiddle[2]=w3;
#if defined(GMF_R4_S3_SIMD)
                for(int t=0;t<4;++t)c.stream[t]=data+2*(j+t*q);
                if(points==length)batch(&c,inverse);
                else {
                    GMF_R4_PROFILE_BEGIN(micro);
                    if(inverse)gmf_r4_s3_boxes_inverse(&c,points/length,4*length);
                    else gmf_r4_s3_boxes_forward(&c,points/length,4*length);
                    GMF_R4_PROFILE_END(micro,GMF_R4_BATCH);
                }
#else
                for(int base=0;base<points;base+=length) {
                    for(int t=0;t<4;++t)c.stream[t]=data+2*(base+j+t*q);
                    batch(&c,inverse);
                }
#endif
            }
        }
#if defined(GMF_R4_S3_SIMD)
        else if(q==2) {
            /*64-bit PIE transfers gather/scatter two length8 boxes directly. */
            _Alignas(16) int16_t w[3][8];
            for(int t=0;t<4;++t){int j=t&1,k=3*j,sign=1;if(k>=half){k-=half;sign=-1;}
                for(int a=0;a<2;++a){w[0][2*t+a]=w1[2*j+a];w[1][2*t+a]=w2[2*j+a];w[2][2*t+a]=sign*w1[2*k+a];}}
            for(int a=0;a<3;++a)c.twiddle[a]=w[a];
            for(int a=0;a<4;++a)c.stream[a]=data+4*a;
            GMF_R4_PROFILE_BEGIN(micro);
            if(inverse)gmf_r4_s3_small8_boxes_inverse(&c,points/16,64);
            else gmf_r4_s3_small8_boxes_forward(&c,points/16,64);
            GMF_R4_PROFILE_END(micro,GMF_R4_BATCH);
        }
#endif
        else {
            _Alignas(16) int16_t v[4][8],w[3][8];
            /* points>=16 on all public experiment sizes, hence a multiple of
             * four butterflies even when q=1 or2. No padded or skipped bin. */
            for(int first=0;first<points/4;first+=4) {
                int16_t* original[4][4];
                for(int t=0;t<4;++t){int index=first+t,base=(index/q)*length,j=index%q;
                    for(int a=0;a<4;++a){original[a][t]=data+2*(base+j+a*q);gmf_copy_complex(v[a]+2*t,original[a][t]);}
                    int k=3*j,sign=1;if(k>=half){k-=half;sign=-1;}
                    for(int a=0;a<2;++a){w[0][2*t+a]=w1[2*j+a];w[1][2*t+a]=w2[2*j+a];w[2][2*t+a]=sign*w1[2*k+a];}}
                for(int a=0;a<4;++a)c.stream[a]=v[a];
                for(int a=0;a<3;++a)c.twiddle[a]=w[a];
                batch(&c,inverse);
                for(int a=0;a<4;++a)for(int t=0;t<4;++t)gmf_copy_complex(original[a][t],v[a]+2*t);
            }
        }
        offset+=length+length/2;length/=4;log2n-=2;
    }
    if(log2n) {
        int s=hp?shift_hp(data,points,2):1,rounding=hp&&s?1<<(s-1):0;extra+=s;
        GMF_R4_PROFILE_BEGIN(tail);
#if defined(GMF_R4_S3_SIMD)
        gmf_r4_s3_unity(data,points,s,rounding);
#else
        for(int i=0;i<points;i+=2){int32_t ar=data[2*i],ai=data[2*i+1],br=data[2*i+2],bi=data[2*i+3];
            data[2*i]=sat((ar+br+rounding)>>s);data[2*i+1]=sat((ai+bi+rounding)>>s);
            data[2*i+2]=sat((ar-br+rounding)>>s);data[2*i+3]=sat((ai-bi+rounding)>>s);}
#endif
        GMF_R4_PROFILE_END(tail,GMF_R4_TAIL);
    }
    GMF_R4_PROFILE_END(start,GMF_R4_COMPLEX);
    return extra;
}
