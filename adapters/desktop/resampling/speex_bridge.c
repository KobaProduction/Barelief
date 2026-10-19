/* Standalone build of unmodified SpeexDSP 1.2.1 resample.c. See vendor/COPYING.
   Local extensions expose bounded state transfer and exact allocated payload. */
#define OUTSIDE_SPEEX
#define FLOATING_POINT
#define RANDOM_PREFIX qtvis_speex
/* Count the vendor's own allocation calls as well as C++ allocations in the
   addressable hot-path regression. Relaxed counters add no allocation. */
#include <stdlib.h>
#include <stdatomic.h>
static _Atomic size_t qtvis_speex_alloc_calls=0;
static void *qtvis_speex_calloc(size_t count,size_t size) {
    atomic_fetch_add_explicit(&qtvis_speex_alloc_calls,1,memory_order_relaxed);return calloc(count,size);
}
static void *qtvis_speex_realloc(void *ptr,size_t size) {
    atomic_fetch_add_explicit(&qtvis_speex_alloc_calls,1,memory_order_relaxed);return realloc(ptr,size);
}
#define calloc qtvis_speex_calloc
#define realloc qtvis_speex_realloc
#include "vendor/resample.c"
#undef calloc
#undef realloc
#include <string.h>
#include <stddef.h>
size_t qtvis_speex_allocation_calls(void) {return atomic_load_explicit(&qtvis_speex_alloc_calls,memory_order_relaxed);}

size_t qtvis_speex_coefficient_bytes(const SpeexResamplerState *s) {
    return s->sinc_table_length*sizeof(*s->sinc_table);
}
size_t qtvis_speex_state_bytes(const SpeexResamplerState *s) {
    return sizeof(*s)+s->nb_channels*(sizeof(*s->last_sample)+sizeof(*s->samp_frac_num)+
        sizeof(*s->magic_samples)+s->mem_alloc_size*sizeof(*s->mem));
}
void qtvis_speex_reset(SpeexResamplerState *s) {
    speex_resampler_reset_mem(s);
    memset(s->mem,0,s->nb_channels*s->mem_alloc_size*sizeof(*s->mem));s->started=0;
}
int qtvis_speex_initial_position(SpeexResamplerState *s,unsigned numerator,unsigned denominator) {
    unsigned channel;unsigned long long scaled;
    if(!s || s->started || !denominator)return 0;
    scaled=(unsigned long long)(numerator%denominator)*s->den_rate;
    if(scaled%denominator || numerator/denominator>INT_MAX)return 0;
    for(channel=0;channel<s->nb_channels;++channel){
        s->last_sample[channel]=(spx_int32_t)(numerator/denominator);
        s->samp_frac_num[channel]=(spx_uint32_t)(scaled/denominator);
    }
    return 1;
}
int qtvis_speex_query(unsigned in,unsigned out,unsigned channels,unsigned quality,
    size_t *coeff,size_t *state,unsigned *delay) {
    unsigned a=in,b=out,gcd,length,over,den,num;size_t table;
    if(!in || !out || !channels || channels>2 || quality>10)return 0;
    while(b){unsigned t=a%b;a=b;b=t;}gcd=a;num=in/gcd;den=out/gcd;
    length=quality_map[quality].base_length;over=quality_map[quality].oversample;
    if(num>den){length=(unsigned)(((unsigned long long)length*num)/den);length=((length-1)&~7u)+8;
        if(2*den<num)over>>=1;
        if(4*den<num)over>>=1;
        if(8*den<num)over>>=1;
        if(16*den<num)over>>=1;
        if(!over)over=1;
    }
    table=(size_t)length*den;
    if(table>(size_t)length*over+8)table=(size_t)length*over+8;
    *coeff=table*sizeof(spx_word16_t);
    *state=sizeof(SpeexResamplerState)+channels*(sizeof(spx_int32_t)+2*sizeof(spx_uint32_t)+(length-1+160)*sizeof(spx_word16_t));
    *delay=in==out?0:length/2;return 1;
}
int qtvis_speex_copy_state(SpeexResamplerState *dst,const SpeexResamplerState *src) {
    if(dst->in_rate!=src->in_rate || dst->out_rate!=src->out_rate || dst->quality!=src->quality ||
       dst->nb_channels!=src->nb_channels || dst->mem_alloc_size!=src->mem_alloc_size ||
       dst->sinc_table_length!=src->sinc_table_length || dst->filt_len!=src->filt_len)return 0;
    memcpy(dst->last_sample,src->last_sample,src->nb_channels*sizeof(*src->last_sample));
    memcpy(dst->samp_frac_num,src->samp_frac_num,src->nb_channels*sizeof(*src->samp_frac_num));
    memcpy(dst->magic_samples,src->magic_samples,src->nb_channels*sizeof(*src->magic_samples));
    memcpy(dst->mem,src->mem,src->nb_channels*src->mem_alloc_size*sizeof(*src->mem));
    dst->started=src->started;return 1;
}
