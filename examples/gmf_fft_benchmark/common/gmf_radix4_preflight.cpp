/* SPDX-License-Identifier: Apache-2.0
 * Firmware-only acceptance gate: execute actual PIE instructions against their
 * host arithmetic semantic model before publishing any performance result. */
#include "gmf_radix4_simd.h"
#include "gmf_optimized.h"
#include "gmf_permutation.h"
#include "esp_gmf_fft_heap.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
namespace {
struct alignas(16) Guarded {
    std::array<int16_t,8> before;
    std::array<int16_t,8> values;
    std::array<int16_t,8> after;
    void reset(){before.fill(0x5312);after.fill(0x5312);}
    bool good()const{return std::all_of(before.begin(),before.end(),[](auto v){return v==0x5312;})&&
        std::all_of(after.begin(),after.end(),[](auto v){return v==0x5312;});}
};
uint32_t seed=0x3a917bd2;
uint32_t random_word(){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
struct AlignedBlock {
    int16_t* allocation;int count;
    explicit AlignedBlock(int n):allocation(static_cast<int16_t*>(esp_gmf_fft_calloc_aligned(n+16,2,16))),count(n){}
    ~AlignedBlock(){if(allocation)esp_gmf_fft_free_aligned(allocation);}
    int16_t* data(){return allocation+8;}
    void guard(){std::fill_n(allocation,8,0x5312);std::fill_n(data()+count,8,0x5312);}
    bool good()const{return std::all_of(allocation,allocation+8,[](auto v){return v==0x5312;})&&
        std::all_of(allocation+8+count,allocation+16+count,[](auto v){return v==0x5312;});}
};
#if defined(GMF_S3_VECTOR_PERMUTATION)
bool permutation() {
    for(int log=3;log<=13;++log){
        const int count=1<<log;AlignedBlock a(2*count),b(2*count);
        if(!a.allocation||!b.allocation)return false;
        for(int fixture=0;fixture<4;++fixture){
            a.guard();b.guard();
            for(int i=0;i<count;++i){
                a.data()[2*i]=fixture==0?int16_t(i):fixture==1?-32768:int16_t(random_word());
                a.data()[2*i+1]=fixture==0?int16_t(~i):fixture==1?32767:int16_t(random_word());
            }
            std::memcpy(b.data(),a.data(),4*count);
            gmf_s3_bit_reverse_s16(a.data(),count,log);
            for(int i=0;i<count;++i){int reversed=0;
                for(int bit=0;bit<log;++bit)reversed=(reversed<<1)|((i>>bit)&1);
                if(a.data()[2*i]!=b.data()[2*reversed]||a.data()[2*i+1]!=b.data()[2*reversed+1]){
                    std::printf("permutation_preflight points=%d fixture=%d index=%d oracle=FAIL\n",count,fixture,i);return false;}
            }
            gmf_opt_bit_reverse_s16(b.data(),count,log);
            if(std::memcmp(a.data(),b.data(),4*count))return false;
            gmf_s3_bit_reverse_s16(a.data(),count,log);
            gmf_opt_bit_reverse_s16(b.data(),count,log);
            if(std::memcmp(a.data(),b.data(),4*count)||!a.good()||!b.good())return false;
        }
    }
    std::puts("permutation_preflight cases=44 complex_points=8..8192 independent_index_oracle=PASS C_bitexact=PASS involution=PASS guards=PASS");return true;
}
#endif
bool box_loops() {
    for(int small=0;small<2;++small)for(int count:{1,2,3,5}){
        AlignedBlock got(32*count),expected(32*count);
        if(!got.allocation||!expected.allocation)return false;
        for(int inverse=0;inverse<2;++inverse)for(int shift=14;shift<=17;++shift)for(int rounded=0;rounded<2;++rounded){
            got.guard();expected.guard();
            for(int i=0;i<got.count;++i)got.data()[i]=int16_t(random_word());
            std::memcpy(expected.data(),got.data(),2*got.count);
            alignas(16) int16_t w[3][8];
            for(int t=0;t<3;++t)for(int lane=0;lane<4;++lane){w[t][2*lane]=11585;w[t][2*lane+1]=lane%2?-11585:11585;}
            gmf_r4_batch c{{0},{w[0],w[1],w[2]},shift,rounded?1<<(shift-3):0};
            for(int a=0;a<4;++a)c.stream[a]=got.data()+(small?4:8)*a;
            const auto original=c;
            if(small){if(inverse)gmf_r4_s3_small8_boxes_inverse(&c,count,64);else gmf_r4_s3_small8_boxes_forward(&c,count,64);}
            else {if(inverse)gmf_r4_s3_boxes_inverse(&c,count,64);else gmf_r4_s3_boxes_forward(&c,count,64);}
            for(int box=0;box<count;++box){
                alignas(16) int16_t streams[4][8];
                gmf_r4_batch ref=c;
                for(int a=0;a<4;++a){ref.stream[a]=streams[a];
                    if(small){std::memcpy(streams[a],expected.data()+32*box+4*a,8);std::memcpy(streams[a]+4,expected.data()+32*box+16+4*a,8);}
                    else std::memcpy(streams[a],expected.data()+32*box+8*a,16);
                }
                gmf_r4_batch_model(&ref,inverse);
                for(int a=0;a<4;++a){
                    if(small){std::memcpy(expected.data()+32*box+4*a,streams[a],8);std::memcpy(expected.data()+32*box+16+4*a,streams[a]+4,8);}
                    else std::memcpy(expected.data()+32*box+8*a,streams[a],16);
                }
            }
            if(std::memcmp(got.data(),expected.data(),2*got.count)||std::memcmp(&c,&original,sizeof(c))||!got.good()||!expected.good()){
                std::printf("box_loop_preflight small=%d count=%d inverse=%d shift=%d rounded=%d FAIL\n",small,count,inverse,shift,rounded);return false;}
        }
    }
    std::puts("box_loop_preflight cases=128 counts=1,2,3,5 normal_small8=PASS model_bitexact=PASS context_unchanged=PASS guards=PASS");return true;
}
bool transforms() {
    constexpr double pi=3.14159265358979323846;
    for(int points:{16,32,64,128,256,512,1024,2048,4096,8192}) {
        AlignedBlock a(2*points+2),b(2*points+2),w(2*points);
        if(!a.allocation||!b.allocation||!w.allocation)return false;
        a.guard();b.guard();w.guard();int log=0,at=0;
        for(int n=points;n>1;n/=2)++log;
        for(int count=points/2;count;count/=2)for(int j=0;j<count;++j){double phase=pi*j/count;
            w.data()[at++]=int16_t(std::lround(16384*std::cos(phase)));
            w.data()[at++]=int16_t(std::lround(16384*std::sin(phase)));}
        for(int pack=0;pack<2;++pack) {
            if(pack)for(int j=0;j<points/2;++j){double phase=-pi*((double)(j+1)/points+.5);
                w.data()[2*j]=int16_t(std::lround(16384*std::cos(phase)));
                w.data()[2*j+1]=int16_t(std::lround(16384*std::sin(phase)));}
            for(int t=0;t<8;++t)for(int hp=0;hp<2;++hp)for(int inverse=0;inverse<2;++inverse) {
                for(int i=0;i<a.count;++i)a.data()[i]=t==0?0:t==1?-32768:t==2?32767:
                    t==3?(i%2?32767:-32768):int16_t(random_word());
                std::memcpy(b.data(),a.data(),2*a.count);int expected=0,actual=0;
                if(pack) {
                    if(hp){expected=gmf_opt_hp4_pack(a.data(),w.data(),points,inverse);
                        actual=gmf_hp4_pack_simd(b.data(),w.data(),points,inverse);}
                    else {if(inverse)gmf_opt_ffti_s16(a.data(),w.data(),points,14);
                        else gmf_opt_fftr_s16(a.data(),w.data(),points,14);
                        gmf_q15_pack_simd(b.data(),w.data(),points,inverse);}
                } else {
                    if(hp)expected=gmf_opt_radix4_hp_s16(a.data(),w.data(),log,points,inverse);
                    else gmf_opt_radix4_s16(a.data(),w.data(),log,points,inverse);
                    actual=gmf_r4_simd(b.data(),w.data(),log,points,inverse,hp);
                }
                if(std::memcmp(a.data(),b.data(),2*a.count)||(hp&&expected!=actual)||!a.good()||!b.good()||!w.good()) {
                    std::printf("simd_preflight transform points=%d pack=%d set=%d hp=%d inverse=%d exponent=%d/%d FAIL\n",
                        points,pack,t,hp,inverse,expected,actual);return false;}
            }
        }
        std::printf("simd_preflight transform points=%d sets=8 CFFT_real_split_Q15_HP_forward_inverse_bitexact=PASS guards=PASS\n",points);
    }
    return true;
}
}
bool gmf_r4_preflight() {
    if(!box_loops())return false;
#if defined(GMF_S3_VECTOR_PERMUTATION)
    if(!permutation())return false;
#endif
    esp_gmf_fft_handle_t plan=nullptr;
    if(gmf_optimized_init(32,&plan)!=ESP_GMF_FFT_OK)return false;
    alignas(16) std::array<int16_t,48> unaligned;
    unaligned.fill(1234);const auto untouched=unaligned;bool rejected=true;
    rejected&=gmf_optimized_forward(plan,unaligned.data()+1,1)==ESP_GMF_FFT_ERR_INVALID_ARG;
    rejected&=gmf_optimized_inverse(plan,unaligned.data()+1,1)==ESP_GMF_FFT_ERR_INVALID_ARG;
    rejected&=gmf_optimized_forward_hp(plan,unaligned.data()+1,1)==ESP_GMF_FFT_ERR_INVALID_ARG;
    rejected&=gmf_optimized_inverse_hp(plan,unaligned.data()+1,1)==ESP_GMF_FFT_ERR_INVALID_ARG;
    esp_gmf_fft_deinit(&plan);
    if(!rejected||unaligned!=untouched){std::puts("simd_preflight alignment_atomic=FAIL");return false;}
    Guarded actual[4],reference[4];
    alignas(16) int16_t w[3][8];
    const int16_t choices[][2]={{16384,0},{11585,11585},{0,16384},{-11585,11585},
        {-16384,0},{-11585,-11585},{0,-16384},{11585,-11585}};
    for(int t=0;t<512;++t) {
        alignas(16) std::array<int16_t,32> peak_values;
        for(auto& v:peak_values)v=int16_t(random_word());
        peak_values[t%32]=-32768;
        for(int count:{8,16,24,32}){int32_t expected_peak=0;
            for(int j=0;j<count;++j){int32_t v=peak_values[j];expected_peak=std::max(expected_peak,v<0?-v:v);}
            if(gmf_r4_s3_peak(peak_values.data(),count)!=expected_peak){std::puts("simd_preflight peak_loop=FAIL");return false;}}
        for(int a=0;a<4;++a){actual[a].reset();reference[a].reset();
            for(int j=0;j<8;++j){int amplitude=t<32?32768:t<64?4096:t<96?8192:0;
                actual[a].values[j]=amplitude?int16_t((t>>(a+j%2))&1?-amplitude:amplitude-(amplitude==32768)):int16_t(random_word());}
            reference[a].values=actual[a].values;
            int32_t peak=0;for(auto v:actual[a].values)peak=std::max(peak,v<0?-int32_t(v):int32_t(v));
            if(gmf_r4_s3_peak(actual[a].values.data(),8)!=peak){std::puts("simd_preflight peak=FAIL");return false;}
        }
        for(auto& v:w)for(int j=0;j<4;++j){const auto& xy=choices[random_word()%8];v[2*j]=xy[0];v[2*j+1]=xy[1];}
        gmf_r4_batch ca{{actual[0].values.data(),actual[1].values.data(),actual[2].values.data(),actual[3].values.data()},
            {w[0],w[1],w[2]},14+t%4,(t/4)%2?1<<(11+t%4):0};
        gmf_r4_batch cr=ca;for(int a=0;a<4;++a)cr.stream[a]=reference[a].values.data();int inverse=(t/8)%2;
        gmf_r4_batch_model(&cr,inverse);
        if(inverse)gmf_r4_s3_batch_inverse(&ca);else gmf_r4_s3_batch_forward(&ca);
        for(int a=0;a<4;++a)if(actual[a].values!=reference[a].values || !actual[a].good()){
            std::printf("simd_preflight batch=%d stream=%d shift=%d bias=%d inverse=%d guards=%d FAIL\n",t,a,int(ca.product_shift),int(ca.bias_quarter),inverse,actual[a].good());
            for(int j=0;j<8;++j) std::printf("simd_difference lane=%d actual=%d expected=%d\n",j,int(actual[a].values[j]),int(reference[a].values[j]));
            return false;}
        struct alignas(16) PairOutput {std::array<int32_t,4> before;std::array<int32_t,16> value;std::array<int32_t,4> after;};
        PairOutput got{},expected{};got.before.fill(0x5312);got.after.fill(0x5312);int bias=(t/4)%2?2048:0;
        auto* a=reference[0].values.data();auto* b=reference[1].values.data();
        gmf_r4_real_pairs_model(a,b,w[0],expected.value.data(),inverse,bias);
        if(inverse)gmf_r4_s3_pairs_inverse(a,b,w[0],got.value.data(),bias);
        else gmf_r4_s3_pairs_forward(a,b,w[0],got.value.data(),bias);
        if(got.value!=expected.value || !std::all_of(got.before.begin(),got.before.end(),[](auto v){return v==0x5312;})||
            !std::all_of(got.after.begin(),got.after.end(),[](auto v){return v==0x5312;})){
            std::printf("simd_preflight real_pairs=%d FAIL\n",t);return false;}
    }
    if(!transforms())return false;
    std::puts("simd_preflight batches=512 real_pairs=512 peak=4096 transforms=640 alignment_atomic=PASS guards=PASS result=PASS");return true;
}
