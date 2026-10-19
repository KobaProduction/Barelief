#include "gmf_optimized.h"
#include "gmf_probe.h"
#include "esp_gmf_fft_heap.h"
#include "test_oracle.h"
#include <cstdio>
#include <cstring>
#include <cmath>
namespace {
constexpr double tau=6.2831853071795864769;
bool check(size_t n) {
    esp_gmf_fft_handle_t plan=nullptr;
    if(gmf_optimized_init(n,&plan)!=ESP_GMF_FFT_OK)return false;
    auto* a=(int16_t*)esp_gmf_fft_calloc_aligned(n+2,2,16);
    auto* b=(int16_t*)esp_gmf_fft_calloc_aligned(n+2,2,16);
    auto* c=(int16_t*)esp_gmf_fft_calloc_aligned(n+2,2,16);
    if(!a||!b||!c)return false;
    std::vector<int16_t> input(n);uint32_t rng=0x1949;
    for(unsigned test=0;test<48;++test) {
        for(size_t i=0;i<n;++i) {
            rng=rng*1664525u+1013904223u;int v=int(rng>>16)-32768;
            if(test==0)v=0;
            if(test==1)v=i==17?24576:0;
            if(test==2)v=4096;
            if(test==3)v=(i&1)?-4096:4096;
            if(test==4)v=int(4096*std::cos(tau*13*i/n+.37)+2000*std::cos(tau*71*i/n+.81));
            if(test==5)v/=4;
            if(test==6)v=int(4096*std::cos(tau*13*i/n+.37)+.4096*std::cos(tau*71*i/n+.81));
            if(test==7)v=(i&1)?-32768:32767;
            input[i]=int16_t(v);
        }
        std::memcpy(a,input.data(),n*2);std::memcpy(b,a,n*2);std::memcpy(c,a,n*2);
        if(n<=8192) {
            esp_gmf_fft_forward_hp(plan,a);const int exp=gmf_probe_exponent(plan);
            gmf_optimized_forward_hp(plan,b,0);
            if(exp!=gmf_probe_exponent(plan)||std::memcmp(a,b,(n+2)*2))return false;
            esp_gmf_fft_inverse_hp(plan,a);gmf_optimized_inverse_hp(plan,b,0);
            if(std::memcmp(a,b,n*2))return false;
        } else if(test<7) {
            gmf_optimized_forward_hp(plan,b,0);
            const double scale=std::ldexp(1.,gmf_probe_exponent(plan)+15);
            auto ref=gmf_test::reference(input);double p=0,e=0;
            for(size_t k=0;k<=n/2;++k) {
                auto value=k==0?std::complex<double>(b[0],0):k==n/2?std::complex<double>(b[1],0):std::complex<double>(b[2*k],b[2*k+1]);
                p+=std::norm(ref[k]);e+=std::norm(value*scale-ref[k]);
            }
            if(p && e && 10*std::log10(p/e)<45)return false;
        }
        gmf_optimized_forward_hp(plan,c,1);const int exponent=gmf_probe_exponent(plan);
        if(test<8) {
            auto ref=gmf_test::reference(input);double p=0,e=0;
            const double scale=std::ldexp(1.,exponent+15);
            for(size_t k=0;k<=n/2;++k) {
                auto value=k==0?std::complex<double>(c[0],0):k==n/2?std::complex<double>(c[1],0):std::complex<double>(c[2*k],c[2*k+1]);
                const double w=k==0||k==n/2?1:2;
                p+=w*std::norm(ref[k]);e+=w*std::norm(value*scale-ref[k]);
            }
            const double snr=e==0?300:p==0?-300:10*std::log10(p/e);
            std::printf("hp4 N=%u fixture=%u forward_snr_db=%.6f exponent=%d\n",unsigned(n),test,snr,exponent);
            if(p && snr<45)return false;
        }
        gmf_optimized_inverse_hp(plan,c,1);
        if(test<8) {
            double p=0,e=0;
            for(size_t i=0;i<n;++i){p+=double(input[i])*input[i];double d=double(c[i])-input[i];e+=d*d;}
            const double snr=e==0?300:p==0?-300:10*std::log10(p/e);
            std::printf("hp4 N=%u fixture=%u roundtrip_snr_db=%.6f\n",unsigned(n),test,snr);
            if(p && snr<40)return false;
        }
        if(!gmf_probe_guards_ok())return false;
    }
    std::memset(c,0,(n+2)*2);gmf_optimized_forward_hp(plan,c,1);
    std::vector<int16_t> spectrum(n+2);
    for(auto& v:spectrum) {rng=rng*1664525u+1013904223u;v=int16_t(int(rng>>24)-128);}
    spectrum[n]=spectrum[1];spectrum[n+1]=0;std::memcpy(c,spectrum.data(),(n+2)*2);
    gmf_optimized_inverse_hp(plan,c,1);
    for(size_t t:{size_t(0),size_t(1),size_t(17),n/4,n/2,n-1}) {
        double ref=spectrum[0]+double(spectrum[1])*(t%2?-1:1);
        for(size_t k=1;k<n/2;++k) {double angle=tau*k*t/n;ref+=2*(spectrum[2*k]*std::cos(angle)-spectrum[2*k+1]*std::sin(angle));}
        if(std::abs(c[t]-ref/n)>3)return false;
    }
    esp_gmf_fft_free_aligned(a);esp_gmf_fft_free_aligned(b);esp_gmf_fft_free_aligned(c);esp_gmf_fft_deinit(&plan);
    std::printf("hp_candidate N=%u tests=48 r2_stock_bit_exact=%s independent_inverse=PASS guards=PASS\n",unsigned(n),n<=8192?"PASS":"unsupported");
    return gmf_probe_live_bytes()==0;
}
}
int main() {
    for(size_t n:{512,1024,2048,4096,8192,16384})if(!check(n)){std::printf("hp_candidate result=FAIL N=%u\n",unsigned(n));return 1;}
    std::printf("hp_candidate result=PASS\n");return 0;
}
