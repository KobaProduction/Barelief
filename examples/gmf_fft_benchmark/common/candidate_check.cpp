#include "test_oracle.h"
#include "gmf_optimized.h"
#include "gmf_probe.h"
#include "esp_gmf_fft_heap.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>
namespace {
constexpr double tau=6.2831853071795864769;
using gmf_test::reference;

double spectrum_error(const int16_t* data,const std::vector<std::complex<double>>& ref,double* maximum=nullptr) {
    double error=0;
    const auto n=ref.size();
    for(size_t k=0;k<=n/2;++k) {
        std::complex<double> value=k==0?std::complex<double>(data[0],0):
            k==n/2?std::complex<double>(data[1],0):std::complex<double>(data[2*k],data[2*k+1]);
        const auto delta=value-ref[k]*(2./n);
        error+=std::norm(delta);
        if(maximum) *maximum=std::max(*maximum,std::abs(delta));
    }
    return error;
}
bool check(size_t n) {
    esp_gmf_fft_handle_t optimized=nullptr,stock=nullptr;
    if(gmf_optimized_init(n,&optimized)!=ESP_GMF_FFT_OK) return false;
    if(n<=8192) {
        esp_gmf_fft_cfg_t cfg{int16_t(n),ESP_GMF_FFT_TYPE_REAL_Q15};
        if(esp_gmf_fft_init(&cfg,&stock)!=ESP_GMF_FFT_OK) return false;
    }
    auto* a=(int16_t*)esp_gmf_fft_calloc_aligned(n+2,sizeof(int16_t),16);
    auto* b=(int16_t*)esp_gmf_fft_calloc_aligned(n+2,sizeof(int16_t),16);
    auto* c=(int16_t*)esp_gmf_fft_calloc_aligned(n+2,sizeof(int16_t),16);
    if(!a||!b||!c) return false;
    std::vector<int16_t> input(n);
    uint32_t rng=0x49a1;
    double e2=0,e4=0;
    for(unsigned test=0;test<40;++test) {
        for(size_t i=0;i<n;++i) {
            rng=rng*1664525u+1013904223u;
            int value=int(rng>>16)-32768;
            if(test<6) {
                if(test==0) value=0;
                if(test==1) value=i==17?24576:0;
                if(test==2) value=4096;
                if(test==3) value=(i&1)?-4096:4096;
                if(test==4) value=int(4096*std::cos(tau*13*i/n+.37)+
                    2000*std::cos(tau*71*i/n+.81));
                if(test==5) value/=4;
            }
            input[i]=int16_t(value);
        }
        std::memcpy(a,input.data(),n*2);std::memcpy(b,a,n*2);std::memcpy(c,a,n*2);
        if(stock) esp_gmf_fft_forward(stock,a);
        gmf_optimized_forward(optimized,b,0);
        if(stock && std::memcmp(a,b,(n+2)*2)) return false;
        gmf_optimized_forward(optimized,c,1);
        if(test<=5) {
            const auto ref=reference(input);
            double max_error4=0;
            const double error2=spectrum_error(b,ref),error4=spectrum_error(c,ref,&max_error4);
            e2+=error2;e4+=error4;
            /* A wrong permutation/twiddle produces O(10^6) error here;
             * this checks every bin, DC and Nyquist, not just peak locations. */
            if(error4>double(n)*20 || max_error4>12) return false;
        }
        if(stock) esp_gmf_fft_inverse(stock,a);
        gmf_optimized_inverse(optimized,b,0);
        if(stock && std::memcmp(a,b,n*2)) return false;
        gmf_optimized_inverse(optimized,c,1);
        if(!gmf_probe_guards_ok()) return false;
    }
    // Independent inverse test: an arbitrary low-level Hermitian spectrum,
    // not a spectrum created by either candidate's own forward transform.
    for(size_t k=0;k<n/2;++k) {
        rng=rng*1664525u+1013904223u;
        a[2*k]=int16_t(int(rng>>24)-128);
        rng=rng*1664525u+1013904223u;
        a[2*k+1]=int16_t(int(rng>>24)-128);
    }
    a[n]=a[1];a[n+1]=0;
    std::vector<int16_t> spectrum(a,a+n+2);
    std::memcpy(b,a,(n+2)*2);std::memcpy(c,a,(n+2)*2);
    gmf_optimized_inverse(optimized,b,0);gmf_optimized_inverse(optimized,c,1);
    double inverse_worst=0;
    for(size_t t:{size_t(0),size_t(1),size_t(3),size_t(17),n/4,n/2,n-1}) {
        double ref=spectrum[0]+double(spectrum[1])*(t%2?-1:1);
        for(size_t k=1;k<n/2;++k) {
            const double angle=tau*k*t/n;
            ref+=2*(double(spectrum[2*k])*std::cos(angle)-double(spectrum[2*k+1])*std::sin(angle));
        }
        ref*=2./n;
        inverse_worst=std::max(inverse_worst,std::abs(double(c[t])-ref));
        if(std::abs(double(c[t])-ref)>12) return false;
    }
    std::printf("inverse_independent N=%u r4_max_q15_lsb_error=%.6f state=PASS\n",unsigned(n),inverse_worst);
    std::printf("candidate N=%u tests=40 r2_stock_bit_exact=%s r2_error_energy=%.6f r4_error_energy=%.6f guards=PASS\n",
        unsigned(n),stock?"PASS":"not_supported",e2,e4);
    esp_gmf_fft_free_aligned(a);esp_gmf_fft_free_aligned(b);esp_gmf_fft_free_aligned(c);
    esp_gmf_fft_deinit(&stock);esp_gmf_fft_deinit(&optimized);
    return gmf_probe_live_bytes()==0;
}
}
int main() {
    for(int peak=0;peak<=32768;++peak){
        int expected=0;const uint64_t bound=4ull*peak*23172;
        while(((bound+(uint64_t(1)<<(13+expected)))>>(14+expected))>32767)++expected;
        int16_t data[64]{};const int16_t sample=peak==32768?-32768:int16_t(peak);
        for(int position:{0,63}){data[position]=sample;
            if(gmf_test_stage_shift_4(data,32)!=expected)return 1;
            data[position]=0;
        }
    }
    std::puts("hp4_stage_shift peaks=32769 first_last=PASS exact_bound=PASS");
    uint32_t rng=0x1949;
    for(unsigned test=0;test<200000;++test) {
        rng=rng*1664525u+1013904223u;
        int32_t re=int32_t(rng%262145)-131072;
        rng=rng*1664525u+1013904223u;
        int32_t im=int32_t(rng%262145)-131072;
        const double phase=tau*(test%8192)/8192;
        const int32_t wr=int32_t(std::lround(16384*std::cos(phase)));
        const int32_t wi=int32_t(std::lround(16384*std::sin(phase)));
        if(std::abs(wr)+std::abs(wi)>23172 ||
           gmf_opt_product_q16(re,im,wr,wi)!=int32_t(((int64_t)re*wr+(int64_t)im*wi)>>16)) {
            std::printf("product_equivalence result=FAIL test=%u\n",test);return 1;
        }
        for(int shift=14;shift<=17;++shift) {
            const auto expected=int32_t(((int64_t)re*wr+(int64_t)im*wi+(int64_t(1)<<(shift-1)))>>shift);
            if(gmf_test_product_round(re,im,wr,wi,shift)!=expected)return 1;
            for(int inverse=0;inverse<2;++inverse){
                const int rounding=(test&1)?1<<(shift-1):0;
                int16_t actual[2];gmf_test_multiply_pair(re,im,wr,wi,actual,inverse,shift,rounding);
                const int64_t v=inverse?wi:-wi;
                const auto r=((int64_t(re)*wr-int64_t(im)*v+rounding)>>shift);
                const auto i=((int64_t(im)*wr+int64_t(re)*v+rounding)>>shift);
                if(actual[0]!=std::clamp<int64_t>(r,-32768,32767)||actual[1]!=std::clamp<int64_t>(i,-32768,32767))return 1;
            }
        }
    }
    for(int32_t re:{-131072,-131071,-1,0,1,131070,131072})
        for(int32_t im:{-131072,-131071,-1,0,1,131070,131072})
            for(int32_t wr:{-16384,0,16384}) for(int32_t wi:{-16384,0,16384})
                if(std::abs(wr)+std::abs(wi)<=23172 &&
                   gmf_opt_product_q16(re,im,wr,wi)!=int32_t(((int64_t)re*wr+(int64_t)im*wi)>>16)) return 1;
    std::printf("product_equivalence q16_random=200000 hp_random=800000 corners=PASS result=PASS\n");
    std::puts("paired_product int64_oracle=1600000 both_directions=PASS shifts14..17=PASS rounding_floor=PASS");
    for(size_t n:{512,1024,2048,4096,8192,16384}) if(!check(n)) {
        std::printf("candidate result=FAIL N=%u\n",unsigned(n));return 1;
    }
    std::printf("candidate result=PASS\n");return 0;
}
