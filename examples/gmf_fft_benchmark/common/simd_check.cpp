/* SPDX-License-Identifier: Apache-2.0 */
#include "gmf_radix4_simd.h"
#include "gmf_optimized.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
namespace {
uint32_t seed=0x728a3b91;
uint32_t random_word(){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int16_t sat(int64_t v){return int16_t(std::clamp<int64_t>(v,-32768,32767));}
bool batches() {
    alignas(16) int16_t v[4][8],w[3][8],expected[4][8];
    gmf_r4_batch c{{v[0],v[1],v[2],v[3]},{w[0],w[1],w[2]},0,0};
    for(unsigned t=0;t<200000;++t) {
        const int s=14+(t%4),inverse=(t/4)%2;
        c.product_shift=s;c.bias_quarter=(t/8)%2?1<<(s-3):0;
        for(int a=0;a<4;++a)for(int j=0;j<8;++j)
            v[a][j]=t<4096?int16_t(((t/16)>>(a+4*(j%2)))&1?-32768:32767):int16_t(random_word());
        for(auto& a:w)for(int j=0;j<4;++j){double phase=double(random_word()%65536)*6.283185307179586/65536;
            a[2*j]=int16_t(std::lround(16384*std::cos(phase)));a[2*j+1]=int16_t(std::lround(16384*std::sin(phase)));}
        for(int j=0;j<4;++j) {
            const int64_t ar=v[0][2*j],ai=v[0][2*j+1],br=v[1][2*j],bi=v[1][2*j+1],
                cr=v[2][2*j],ci=v[2][2*j+1],dr=v[3][2*j],di=v[3][2*j+1],sign=inverse?-1:1;
            const int64_t re[]={ar+br+cr+dr,ar-br+cr-dr,ar-cr+sign*(bi-di),ar-cr-sign*(bi-di)};
            const int64_t im[]={ai+bi+ci+di,ai-bi+ci-di,ai-ci-sign*(br-dr),ai-ci+sign*(br-dr)};
            int sum_bias=c.bias_quarter>>12;
            expected[0][2*j]=sat((re[0]+sum_bias)>>(s-14));expected[0][2*j+1]=sat((im[0]+sum_bias)>>(s-14));
            for(int a=1;a<4;++a){int k=a==1?1:a==2?0:2;int64_t wr=w[k][2*j],wi=sign*w[k][2*j+1],bias=4*c.bias_quarter;
                expected[a][2*j]=sat((re[a]*wr+im[a]*wi+bias)>>s);
                expected[a][2*j+1]=sat((im[a]*wr-re[a]*wi+bias)>>s);}
        }
        gmf_r4_batch_model(&c,inverse);
        for(int a=0;a<4;++a)for(int j=0;j<8;++j)if(v[a][j]!=expected[a][j]) {
            std::printf("batch t=%u S=%d inverse=%d a=%d j=%d actual=%d expected=%d FAIL\n",t,s,inverse,a,j,v[a][j],expected[a][j]);return false;}
    }
    std::puts("simd_batch int64_oracle=200000 both_directions=1 shift14_to17=1 rounded_and_floor=1 PASS");return true;
}
bool transforms() {
    for(int points:{16,32,64,256,512,1024,2048,4096,8192}) {
        std::vector<int16_t> table(2*points),input(2*points),a(2*points),b(2*points);int at=0,log=0;
        for(int n=points;n>1;n/=2)++log;
        for(int count=points/2;count;count/=2)for(int j=0;j<count;++j){double phase=3.14159265358979323846*j/count;
            table[at++]=int16_t(std::lround(16384*std::cos(phase)));table[at++]=int16_t(std::lround(16384*std::sin(phase)));}
        for(int t=0;t<48;++t) {
            for(size_t i=0;i<input.size();++i)input[i]=t==0?0:t==1?-32768:t==2?32767:t==3?(i%2?32767:-32768):int16_t(random_word());
            for(int hp=0;hp<2;++hp)for(int inverse=0;inverse<2;++inverse){a=input;b=input;
                int expected=hp?gmf_opt_radix4_hp_s16(a.data(),table.data(),log,points,inverse):0;
                if(!hp)gmf_opt_radix4_s16(a.data(),table.data(),log,points,inverse);
                int actual=gmf_r4_simd(b.data(),table.data(),log,points,inverse,hp);
                if(a!=b || (hp&&actual!=expected)) {std::printf("simd_transform points=%d t=%d hp=%d inverse=%d exp=%d/%d FAIL\n",points,t,hp,inverse,expected,actual);return false;}
            }
        }
        std::printf("simd_transform points=%d sets=48 Q15_HP_forward_inverse_bitexact=PASS\n",points);
    }
    return true;
}
bool packs() {
    for(int points:{16,32,64,256,512,1024,2048,4096,8192}) {
        std::vector<int16_t> w(points),input(2*points+2),a(input.size()),b(input.size());
        for(int j=0;j<points/2;++j){double phase=-3.14159265358979323846*((double)(j+1)/points+0.5);
            w[2*j]=int16_t(std::lround(16384*std::cos(phase)));w[2*j+1]=int16_t(std::lround(16384*std::sin(phase)));}
        for(int t=0;t<48;++t){for(auto& v:input)v=t==0?0:t==1?-32768:t==2?32767:int16_t(random_word());
            for(int hp=0;hp<2;++hp)for(int inverse=0;inverse<2;++inverse){a=input;b=input;
                int expected=hp?gmf_opt_hp4_pack(a.data(),w.data(),points,inverse):0;
                if(!hp){if(inverse)gmf_opt_ffti_s16(a.data(),w.data(),points,14);else gmf_opt_fftr_s16(a.data(),w.data(),points,14);}
                int actual=hp?gmf_hp4_pack_simd(b.data(),w.data(),points,inverse):0;
                if(!hp)gmf_q15_pack_simd(b.data(),w.data(),points,inverse);
                if(a!=b || (hp&&actual!=expected)){std::printf("simd_pack points=%d t=%d hp=%d inverse=%d exp=%d/%d FAIL\n",points,t,hp,inverse,expected,actual);return false;}
            }
        }
        std::printf("simd_pack points=%d sets=48 Q15_HP_forward_inverse_bitexact=PASS\n",points);
    }
    return true;
}
}
int main(){bool ok=batches()&&transforms()&&packs();std::printf("simd_model result=%s\n",ok?"PASS":"FAIL");return ok?0:1;}
