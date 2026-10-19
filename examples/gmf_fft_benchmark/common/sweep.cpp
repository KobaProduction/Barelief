#include <dsp_kernels/dsp.h>
#include "fft_benchmark_access.h"
#include "gmf_optimized.h"
#include "gmf_probe.h"
#include "gmf_radix4_profile.h"
#include "esp_gmf_fft_heap.h"
#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_task.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#endif
#if defined(GMF_R4_S3_SIMD)
bool gmf_r4_preflight();
#endif
#if defined(GMF_R4_STAGE_PROFILE)
#define GMF_TIMING_TAG "diagnostic_timing"
#else
#define GMF_TIMING_TAG "timing"
#endif
namespace {
constexpr double tau=6.2831853071795864769;
constexpr unsigned batches=24;
volatile float sink;
volatile int16_t fixed_sink;
double clock_us() {
#if defined(ESP_PLATFORM)
    return double(esp_timer_get_time());
#else
    return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
void yield() {
#if defined(ESP_PLATFORM)
    vTaskDelay(1);
#endif
}
size_t free_heap() {
#if defined(ESP_PLATFORM)
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
#else
    return 0;
#endif
}
bool heap_good() {
#if defined(ESP_PLATFORM)
    return heap_caps_check_integrity_all(true);
#else
    return true;
#endif
}
template<class Setup,class Call> std::array<double,2> timing(size_t n,Setup setup,Call call) {
#if defined(ESP_PLATFORM)
    const unsigned count=n<=2048?8:n<=4096?4:2;
#else
    const unsigned count=n<=2048?512:n<=4096?256:n<=8192?128:64;
#endif
    for(unsigned i=0;i<8;++i) {setup();call();}
    std::array<double,batches> times{};
    for(auto& t:times) {
        t=0;
        for(unsigned i=0;i<count;++i) {
            setup();
            const double begin=clock_us();
            call();
            t+=clock_us()-begin;
        }
        t/=count;
        yield();
    }
    std::sort(times.begin(),times.end());
    return {(times[batches/2-1]+times[batches/2])*.5,times[batches*95/100]};
}
int16_t sample(size_t i,size_t n,unsigned fixture,uint32_t& rng) {
    double x=0;
    constexpr std::array tones{3.,11.25,23.,41.5,67.,89.75,111.};
    if(fixture==0) {
        for(size_t j=0;j<tones.size();++j) x+=.25/(j+1)*std::cos(tau*tones[j]*i/n+.13*j);
        x*=.5-.5*std::cos(tau*i/(n-1));
    } else if(fixture==1) x=.125;
    else if(fixture==2) x=(i&1)?-.125:.125;
    else if(fixture==3) {
        rng=rng*1664525u+1013904223u;
        x=(double(rng)/4294967296.-.5)*.4;
    } else if(fixture==4) {
        x=.125*std::cos(tau*13*i/n+.31)+.0000125*std::cos(tau*71*i/n+.77);
    } else if(fixture==5) x=i==17?.75:0;
    return int16_t(std::clamp(std::lround(x*32768.),-32768l,32767l));
}
template<class Buffer> void fill(Buffer& input,unsigned fixture) {
    using T=std::remove_cvref_t<decltype(input[0])>;
    uint32_t rng=0x1949;
    for(size_t i=0;i<input.size();++i) {
        auto value=sample(i,input.size(),fixture,rng);
        if constexpr(std::is_same_v<T,float>) input[i]=float(value)/32768.f;
        else input[i]=value;
    }
}
template<class Buffer> std::complex<double> dft(const Buffer& input,size_t k) {
    using T=std::remove_cvref_t<decltype(input[0])>;
    auto step=std::polar(1.,-tau*k/input.size());
    std::complex<double> sum=0,w=1;
    for(auto value:input) {
        double x=double(value);
        if constexpr(!std::is_same_v<T,float>) x/=32768.;
        sum+=x*w;w*=step;
    }
    return sum;
}
std::array<size_t,16> check_bins(size_t n) {return {0,1,3,7,11,13,23,41,67,71,89,111,127,n/4,n/2-1,n/2};}
template<class Buffer,class Bin> void noise_error(const Buffer& input,const char* route,Bin bin) {
    using T=std::remove_cvref_t<decltype(input[0])>;
    const size_t n=input.size();
    std::vector<std::complex<double>> ref(n);
    for(size_t i=0;i<n;++i) ref[i]=double(input[i])/(std::is_same_v<T,float>?1.:32768.);
    for(size_t i=1,j=0;i<n;++i) {
        size_t bit=n/2;
        while(j&bit) {j^=bit;bit/=2;}
        j^=bit;if(i<j) std::swap(ref[i],ref[j]);
    }
    for(size_t len=2;len<=n;len*=2) {
        const auto step=std::polar(1.,-tau/len);
        for(size_t base=0;base<n;base+=len) {
            std::complex<double> w=1;
            for(size_t j=0;j<len/2;++j) {
                auto u=ref[base+j],v=ref[base+j+len/2]*w;
                ref[base+j]=u+v;ref[base+j+len/2]=u-v;w*=step;
            }
        }
    }
    double p=0,e=0,oracle_error=0;
    for(auto k:check_bins(n)) oracle_error=std::max(oracle_error,std::abs(ref[k]-dft(input,k)));
    for(size_t k=0;k<=n/2;++k) {
        const double weight=k==0||k==n/2?1:2;
        p+=weight*std::norm(ref[k]);e+=weight*std::norm(bin(k)-ref[k]);
    }
    const double rms=std::sqrt(e)/n;
    std::printf("noise N=%u route=%s weighted_snr_db=%.6f rms_error_fs=%.12g rms_error_dbfs=%.6f oracle_max_error=%.9g\n",
        unsigned(n),route,e==0?300:10*std::log10(p/e),rms,rms==0?-300:20*std::log10(rms),oracle_error);
}
template<class Buffer,class Bin> void weak_error(const Buffer& input,const char* route,Bin bin) {
    const auto reference=dft(input,71),strong=dft(input,13),value=bin(71);
    const double ratio=std::abs(value)/std::max(1e-30,std::abs(reference));
    std::printf("weak N=%u route=%s error_db=%.6f phase_error_rad=%.6f reference_dbc=%.6f\n",
        unsigned(input.size()),route,ratio>0?20*std::log10(ratio):-300,
        std::remainder(std::arg(value)-std::arg(reference),tau),20*std::log10(std::abs(reference)/std::abs(strong)));
}
size_t reverse_bytes(size_t n,bool radix4) {
    const auto exponent=std::countr_zero(n);
    if(radix4) return exponent>=4&&exponent<=12?
        4*size_t(dsps_fft4r_rev_tables_fc32_size[exponent/2-2]):0;
    return exponent>=4&&exponent<=12?4*size_t(dsps_fft2r_rev_tables_fc32_size[exponent-4]):0;
}
size_t shared_bytes(size_t n,bool radix4) {return n*(radix4?16:4)+reverse_bytes(n,radix4);}
void deinit(bool radix4) {
    if(radix4) {dsps_fft4r_deinit_fc32();dsps_fft4r_w_table_fc32=nullptr;dsps_fft4r_w_table_size=0;}
    else {dsps_fft2r_deinit_fc32();dsps_fft_w_table_fc32=nullptr;dsps_fft_w_table_size=0;}
}
bool admitted(size_t n,const char* route,size_t bytes,size_t largest) {
#if defined(ESP_PLATFORM)
    const auto available=free_heap();
    const auto block=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(bytes+4096>available || largest+64>block) {
        std::printf("status N=%u route=%s state=NO_RAM required=%u available=%u largest=%u block=%u reserve=4096\n",
            unsigned(n),route,unsigned(bytes),unsigned(available),unsigned(largest),unsigned(block));
        return false;
    }
#else
    (void)n;(void)route;(void)bytes;(void)largest;
#endif
    return true;
}
template<class Plan> bool floating(size_t n,bool radix4,const char* route) {
    constexpr bool real=std::is_same_v<Plan,dsp_kernels::PreparedRfft>;
    const bool mixed=real&&radix4&&!(n/2&0x55555555);
    const size_t kernel_n=real?(mixed?n/4:n/2):n;
    const size_t workspace=real?6*n+16+(mixed?4*n:0):8*n;
    const size_t tables=shared_bytes(kernel_n,radix4);
    const size_t caller=8*n+8;
    const size_t total=workspace+tables+caller;
    const size_t largest=std::max(n*(real?4:8)+8,kernel_n*(radix4?16:4));
    if(!admitted(n,route,total,largest)) return true;
    const auto before=free_heap();
    const auto init=radix4?dsps_fft4r_init_fc32(nullptr,int(kernel_n)):dsps_fft2r_init_fc32(nullptr,int(kernel_n));
    if(init!=ESP_OK) {deinit(radix4);std::printf("status N=%u route=%s state=INIT_FAILED\n",unsigned(n),route);return false;}
    bool good=true;
    const int result=[&]() -> int {
        Plan plan;
        dsp_kernels::detail::FftStorage input,output;
        if(!plan.prepare(n,radix4) || !input.resize(n) || !output.resize(n+2)) {
            std::printf("status N=%u route=%s state=NO_RAM required=%u available=%u reason=fragmented_allocation\n",
                unsigned(n),route,unsigned(total),unsigned(before));
            return 2;
        }
        const auto delta=before-free_heap();
        std::printf("memory N=%u route=%s workspace=%u shared=%u caller=%u total=%u actual_heap_delta=%u state=FIT\n",
            unsigned(n),route,unsigned(plan.workspace_bytes()),unsigned(tables),unsigned(caller),unsigned(total),unsigned(delta));
        for(unsigned f=0;f<7;++f) {
            fill(input,f);
            if(!plan.process(input,output)) return false;
            dsp_kernels::detail::FftBenchmarkAccess::stage(plan,input);
            if(!dsp_kernels::detail::FftBenchmarkAccess::core(plan)) return false;
            const auto core_out=dsp_kernels::detail::FftBenchmarkAccess::output(plan);
            if(std::memcmp(output.data(),core_out.data(),(n+2)*sizeof(float))) return false;
            double worst=0;
            for(auto k:check_bins(n)) {
                const auto ref=dft(input,k);
                const auto value=std::complex<double>(output[2*k],output[2*k+1]);
                const double error=std::abs(value-ref);worst=std::max(worst,error);
                if(error>.001+1e-5*std::abs(ref)) good=false;
            }
            std::printf("accuracy N=%u route=%s fixture=%u max_selected_raw_error=%.9g state=%s\n",
                unsigned(n),route,f,worst,good?"PASS":"FAIL");
            if(n==4096 && f==3) noise_error(input,route,[&](size_t k){
                return std::complex<double>(output[2*k],output[2*k+1]);});
            if(n==4096 && f==4) weak_error(input,route,[&](size_t k){
                return std::complex<double>(output[2*k],output[2*k+1]);});
            yield();
        }
        fill(input,0);
        const auto core=timing(n,[&]{dsp_kernels::detail::FftBenchmarkAccess::stage(plan,input);},[&]{
            good&=dsp_kernels::detail::FftBenchmarkAccess::core(plan);
            sink=dsp_kernels::detail::FftBenchmarkAccess::output(plan)[13];});
        const auto full=timing(n,[]{},[&]{good&=plan.process(input,output);sink=output[13];});
        std::printf(GMF_TIMING_TAG " N=%u route=%s core_us=%.6f core_p95_us=%.6f full_us=%.6f full_p95_us=%.6f\n",
            unsigned(n),route,core[0],core[1],full[0],full[1]);
        return 1;
    }();
    deinit(radix4);
    std::printf("cleanup N=%u route=%s before=%u after=%u guards=%d\n",unsigned(n),route,unsigned(before),unsigned(free_heap()),heap_good());
    return result!=0&&good&&before==free_heap()&&heap_good();
}
bool fixed(size_t n,unsigned variant) {
    constexpr std::array names{"GMF_Q15","GMF_HP","GMF2R_OPT","GMF4R_OPT","GMF_HP2R_OPT","GMF_HP4R_OPT"};
    const auto route=names[variant];
    const bool hp=variant==1||variant>=4,optimized=variant>=2,radix4=variant==3||variant==5;
    if(n>8192&&!optimized) {
        std::printf("status N=%u route=%s state=UNSUPPORTED_STOCK_MAX8192\n",unsigned(n),route);return true;
    }
    const size_t total=7*n+gmf_probe_plan_bytes()+4;
    if(!admitted(n,route,total+2*n,2*n)) return true;
    const auto before=free_heap();
    esp_gmf_fft_handle_t plan=nullptr;
    esp_gmf_fft_cfg_t cfg{int16_t(n),ESP_GMF_FFT_TYPE_REAL_Q15};
    if((optimized?gmf_optimized_init(n,&plan):esp_gmf_fft_init(&cfg,&plan))!=ESP_GMF_FFT_OK) return false;
    auto* data=(int16_t*)esp_gmf_fft_calloc_aligned(n+2,sizeof(int16_t),16);
    if(!data) {esp_gmf_fft_deinit(&plan);return false;}
    bool good=true;
    {
        std::vector<int16_t> input(n);
        const auto delta=before-free_heap();
        auto forward=[&]{return (optimized?(hp?gmf_optimized_forward_hp(plan,data,radix4):gmf_optimized_forward(plan,data,radix4)):
            hp?esp_gmf_fft_forward_hp(plan,data):esp_gmf_fft_forward(plan,data))==ESP_GMF_FFT_OK;};
        auto inverse=[&]{return (optimized?(hp?gmf_optimized_inverse_hp(plan,data,radix4):gmf_optimized_inverse(plan,data,radix4)):
            hp?esp_gmf_fft_inverse_hp(plan,data):esp_gmf_fft_inverse(plan,data))==ESP_GMF_FFT_OK;};
        auto copy=[&]{std::memcpy(data,input.data(),2*n);};
        std::printf("memory N=%u route=%s workspace=%u shared=0 caller=%u total=%u actual_heap_delta=%u state=FIT\n",
            unsigned(n),route,unsigned(5*n+gmf_probe_plan_bytes()+4),unsigned(2*n),unsigned(total),unsigned(delta));
        for(unsigned f=0;f<7;++f) {
            fill(input,f);copy();if(!forward()) return false;
            if((variant==2||variant==4) && n<=8192) {
                auto* expected=(int16_t*)esp_gmf_fft_calloc_aligned(n+2,sizeof(int16_t),16);
                if(!expected) return false;
                std::memcpy(expected,input.data(),2*n);
                const auto exponent=gmf_probe_exponent(plan);
                if((hp?esp_gmf_fft_forward_hp(plan,expected):esp_gmf_fft_forward(plan,expected))!=ESP_GMF_FFT_OK ||
                   std::memcmp(expected,data,2*(n+2))) return false;
                if(hp && gmf_probe_exponent(plan)!=exponent)return false;
                esp_gmf_fft_free_aligned(expected);
            }
            const double scale=std::ldexp(1.,hp?gmf_probe_exponent(plan):int(std::countr_zero(n/2))-15);
            double p=0,e=0,worst=0;
            for(auto k:check_bins(n)) {
                const auto ref=dft(input,k);
                const auto value=k==0?std::complex<double>(data[0]*scale,0):k==n/2?
                    std::complex<double>(data[1]*scale,0):std::complex<double>(data[2*k]*scale,data[2*k+1]*scale);
                const double error=std::abs(value-ref);worst=std::max(worst,error);
                p+=std::norm(ref);e+=error*error;
            }
            std::printf("accuracy N=%u route=%s fixture=%u selected_snr_db=%.6f max_selected_raw_error=%.9g exponent=%d\n",
                unsigned(n),route,f,e==0?300:p==0?-300:10*std::log10(p/e),worst,gmf_probe_exponent(plan));
            if(n==4096 && f==3) noise_error(input,route,[&](size_t k){
                return k==0?std::complex<double>(data[0]*scale,0):k==n/2?
                    std::complex<double>(data[1]*scale,0):std::complex<double>(data[2*k]*scale,data[2*k+1]*scale);});
            if(n==4096 && f==4) weak_error(input,route,[&](size_t k){
                return std::complex<double>(data[2*k]*scale,data[2*k+1]*scale);});
            if(!inverse()) return false;
            if((variant==2||variant==4) && n<=8192) {
                auto* expected=(int16_t*)esp_gmf_fft_calloc_aligned(n+2,2,16);
                if(!expected)return false;
                std::memcpy(expected,input.data(),2*n);
                if(hp){esp_gmf_fft_forward_hp(plan,expected);esp_gmf_fft_inverse_hp(plan,expected);}
                else {esp_gmf_fft_forward(plan,expected);esp_gmf_fft_inverse(plan,expected);}
                if(std::memcmp(data,expected,n*2))return false;
                esp_gmf_fft_free_aligned(expected);
            }
            double rp=0,re=0;
            for(size_t i=0;i<n;++i) {
                const double error=double(data[i])*(hp?1.:double(n)/4)-input[i];
                rp+=double(input[i])*input[i];re+=error*error;
            }
            std::printf("roundtrip N=%u route=%s fixture=%u snr_db=%.6f\n",unsigned(n),route,f,re==0?300:rp==0?-300:10*std::log10(rp/re));
            yield();
        }
        fill(input,0);
#if defined(GMF_R4_STAGE_PROFILE)
        if(n==4096 && optimized) {
            for(int direction=0;direction<2;++direction) {
                gmf_r4_profile p{};p.clock=+[]()->uint64_t{return esp_cpu_get_cycle_count();};uint64_t elapsed=0;
                for(int repeat=0;repeat<8;++repeat){copy();if(direction&&!forward())return false;
                    gmf_r4_profile_set(&p);auto start=p.clock();bool ok=direction?inverse():forward();
                    elapsed+=uint32_t(p.clock())-uint32_t(start);gmf_r4_profile_set(nullptr);if(!ok)return false;}
                const auto complex_parts=p.ticks[GMF_R4_PEAK]+p.ticks[GMF_R4_BATCH]+p.ticks[GMF_R4_TAIL];
                const auto complex_other=p.ticks[GMF_R4_COMPLEX]>complex_parts?p.ticks[GMF_R4_COMPLEX]-complex_parts:0;
                const auto real_other=p.ticks[GMF_R4_REAL]>p.ticks[GMF_R4_PAIRS]?p.ticks[GMF_R4_REAL]-p.ticks[GMF_R4_PAIRS]:0;
                const double scale=8.*CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
                std::printf("profile N=4096 route=%s direction=%s unit=us inplace=%.3f complex=%.3f peak=%.3f batch=%.3f tail=%.3f layout_other=%.3f real=%.3f pairs=%.3f real_other=%.3f permutation=%.3f detail=%s repeats=8 clock=cycles instrumentation=1 performance_claim=none\n",
                    route,direction?"inverse":"forward",elapsed/scale,p.ticks[GMF_R4_COMPLEX]/scale,p.ticks[GMF_R4_PEAK]/scale,
                    p.ticks[GMF_R4_BATCH]/scale,p.ticks[GMF_R4_TAIL]/scale,complex_other/scale,p.ticks[GMF_R4_REAL]/scale,p.ticks[GMF_R4_PAIRS]/scale,real_other/scale,
                    p.ticks[GMF_R4_PERMUTATION]/scale,radix4?"r4_stages":"permutation_only");
            }
        }
#endif
        const auto allocations=gmf_probe_allocations();
        const auto core=timing(n,copy,[&]{good&=forward();fixed_sink=data[13];});
        const auto full=timing(n,[]{},[&]{copy();good&=forward();fixed_sink=data[13];});
        copy();if(!forward()) return false;
        std::vector<int16_t> spectrum(data,data+n+2);
        auto restore=[&]{std::memcpy(data,spectrum.data(),(n+2)*2);};
        const auto inverse_core=timing(n,restore,[&]{good&=inverse();fixed_sink=data[13];});
        const auto inverse_full=timing(n,[]{},[&]{restore();good&=inverse();fixed_sink=data[13];});
        good&=allocations==gmf_probe_allocations();
        std::printf(GMF_TIMING_TAG " N=%u route=%s core_us=%.6f core_p95_us=%.6f full_us=%.6f full_p95_us=%.6f inverse_core_us=%.6f inverse_core_p95_us=%.6f inverse_full_us=%.6f inverse_full_p95_us=%.6f hot_allocations=%u\n",
            unsigned(n),route,core[0],core[1],full[0],full[1],inverse_core[0],inverse_core[1],inverse_full[0],inverse_full[1],unsigned(gmf_probe_allocations()-allocations));
    }
    esp_gmf_fft_free_aligned(data);esp_gmf_fft_deinit(&plan);
    std::printf("cleanup N=%u route=%s before=%u after=%u guards=%d\n",unsigned(n),route,unsigned(before),unsigned(free_heap()),gmf_probe_guards_ok());
    return good&&before==free_heap()&&gmf_probe_guards_ok()&&gmf_probe_live_bytes()==0&&heap_good();
}
bool run() {
#if defined(GMF_R4_S3_SIMD)
    if(!::gmf_r4_preflight()) return false;
    std::puts("backend r4=PIE_EXPERIMENTAL vector_butterflies=1 vector_real_split=1");
#if defined(GMF_R4_STAGE_PROFILE)
    std::puts("profile_enabled instrumentation=1 use_separate_OFF_image_for_performance=1");
#endif
#elif defined(GMF_R4_ROUTE_MODEL)
    std::puts("backend r4=HOST_SEMANTIC_MODEL performance_claim=none");
#else
    std::puts("backend r4=PORTABLE_C");
#endif
#if defined(_WIN32)
    if(!SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1))) return false;
    std::printf("affinity logical_cpu=0 thread_only=1\n");
#endif
#if defined(ESP_PLATFORM)
    std::printf("sweep target=%s cpu_mhz=%d psram_used=0\n",CONFIG_IDF_TARGET,CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
#else
    std::printf("sweep target=windows compiler=%s backend=ANSI split=SSE2_if_available\n",__VERSION__);
#endif
#if defined(GMF_R4_STAGE_PROFILE) || defined(GMF_BENCHMARK_4096_ONLY)
    constexpr std::array<size_t,1> sizes{4096};
    constexpr auto size_text="4096";
#else
    constexpr std::array<size_t,6> sizes{512,1024,2048,4096,8192,16384};
    constexpr auto size_text="512,1024,2048,4096,8192,16384";
#endif
    std::printf("scope sizes=%s batches=24 warmups=8 timed_calls=MCU2_to8_PC64_to512 clock=per_call reset_outside_core=1 window=excluded output_adapter=excluded native_fixed=Q15 native_float=Float32 shared_tables=minimum_per_route\n",size_text);
    const auto overhead=timing(512,[]{},[]{sink=0;});
    std::printf("timer empty_us=%.6f empty_p95_us=%.6f subtraction=none\n",overhead[0],overhead[1]);
    // Initialize process-lifetime backend synchronization before per-route
    // teardown accounting. On IDF std::mutex lazily owns a semaphore; that
    // permanent resource is neither a table leak nor per-plan workspace.
    const auto bootstrap_before=free_heap();
    if(dsps_fft2r_init_fc32(nullptr,8)!=ESP_OK) return false;
    {
        dsp_kernels::FFT bootstrap;
        if(!bootstrap.prepare(8)) return false;
    }
    deinit(false);
    std::printf("bootstrap persistent_backend_bytes=%u free_after=%u\n",
        unsigned(bootstrap_before-free_heap()),unsigned(free_heap()));
    for(size_t n:sizes) {
        if(!floating<dsp_kernels::FFT>(n,false,"FFT2R")) return false;
        if(n&0x55555555) {if(!floating<dsp_kernels::FFT>(n,true,"FFT4R")) return false;}
        else std::printf("status N=%u route=FFT4R state=UNSUPPORTED_POWER4\n",unsigned(n));
        if(!floating<dsp_kernels::PreparedRfft>(n,false,"RFFT2R")) return false;
        if(!floating<dsp_kernels::PreparedRfft>(n,true,"RFFT4R")) return false;
        for(unsigned variant=0;variant<6;++variant) if(!fixed(n,variant)) return false;
    }
    return true;
}
bool entry() {const bool good=run();
#if defined(ESP_PLATFORM)
    const auto free_stack=uxTaskGetStackHighWaterMark(nullptr);
    std::printf("stack scope=whole_benchmark allocated_bytes=%u minimum_free_bytes=%u maximum_used_bytes=%u\n",
        unsigned(ESP_TASK_MAIN_STACK),unsigned(free_stack),unsigned(ESP_TASK_MAIN_STACK-free_stack));
#endif
    std::printf("fft_benchmark result=%s\n",good?"PASS":"FAIL");return good;}
}
#if defined(ESP_PLATFORM)
extern "C" void app_main() {entry();}
#else
int main() {return entry()?0:1;}
#endif
