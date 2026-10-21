#include <dsp_kernels/dsp.h>
#include "gmf_probe.h"
#include "esp_gmf_fft_heap.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
extern "C" void fft_radix2_bit_reverse_s16(int16_t*, int32_t, int32_t);
#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#endif

namespace {
constexpr double tau = 6.283185307179586476925286766559;
constexpr unsigned batches = 96, batch_size = 8;
volatile float sink;
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
bool platform_heap_ok() {
#if defined(ESP_PLATFORM)
    return heap_caps_check_integrity_all(true);
#else
    return true;
#endif
}
std::size_t free_heap() {
#if defined(ESP_PLATFORM)
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return 0;
#endif
}
template<class F> std::array<double,2> timing(F call) {
    for (unsigned i=0;i<24;++i) call();
    std::array<double,batches> times{};
    for (auto& t:times) {
        double start=clock_us();
        for(unsigned i=0;i<batch_size;++i) call();
        t=(clock_us()-start)/batch_size;
        yield();
    }
    std::sort(times.begin(),times.end());
    return {times[batches/2],times[batches*95/100]};
}
void reverse_oracle(std::vector<std::complex<double>>& a) {
    const auto n=a.size();
    for(std::size_t i=1,j=0;i<n;++i) {
        std::size_t bit=n>>1;
        for(;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if(i<j) std::swap(a[i],a[j]);
    }
}
void oracle_stage(std::vector<std::complex<double>>& a,std::size_t len) {
    auto step=std::polar(1.,-tau/double(len));
    for(std::size_t i=0;i<a.size();i+=len) {
        std::complex<double> w=1;
        for(std::size_t j=0;j<len/2;++j) {
            auto u=a[i+j],v=a[i+j+len/2]*w;
            a[i+j]=u+v; a[i+j+len/2]=u-v; w*=step;
        }
    }
}
// Independent double CFFT oracle. Only used outside measurement, no SDK calls.
std::vector<std::complex<double>> oracle(const std::vector<float>& x) {
    std::size_t n=x.size();
    std::vector<std::complex<double>> a(n);
    for(std::size_t i=0;i<n;++i) a[i]=x[i];
    reverse_oracle(a);
    for(std::size_t len=2;len<=n;len<<=1) oracle_stage(a,len);
    return a;
}
bool validate_oracle(const std::vector<float>& x,const std::vector<std::complex<double>>& a) {
    std::array<std::size_t,16> bins{0,1,3,7,11,13,23,41,67,71,89,111,127,x.size()/4,x.size()/2-1,x.size()/2};
    double error=0;
    for(auto k:bins) {
        auto step=std::polar(1.,-tau*double(k)/x.size());
        std::complex<double> w=1,s=0;
        for(auto v:x) {s+=double(v)*w; w*=step;}
        error=std::max(error,std::abs(s-a[k]));
    }
    std::printf("oracle N=%u direct_dft_max=%.9g\n",unsigned(x.size()),error);
    return error<1e-7;
}
struct Signal { std::vector<float> ideal,input; std::vector<int16_t> pcm; };
double multitone_sample(std::size_t i,std::size_t n) {
    constexpr std::array bins{3.,11.25,23.,41.5,67.,89.75,111.};
    double x=0;
    for(std::size_t j=0;j<bins.size();++j)
        x+=.25/double(j+1)*std::cos(tau*bins[j]*i/n+.13*j);
    return x*(.5-.5*std::cos(tau*i/(n-1)));
}
double named_sample(std::size_t i,std::size_t n,const char* name,const char* kind,std::uint32_t& rng) {
    if(!std::strcmp(name,"multitone")) return multitone_sample(i,n);
    if(!std::strcmp(kind,"noise")) {
        rng=1664525u*rng+1013904223u;
        return (double(rng)/4294967296.-.5)*1.6;
    }
    if(!std::strcmp(kind,"dc")) return .75;
    if(!std::strcmp(name,"nyquist")) return i%2?-.75:.75;
    if(!std::strcmp(name,"impulse")) return i==17?.75:0;
    return 0;
}
void quantize_sample(Signal& s,std::size_t i,double x) {
    s.ideal[i]=float(x);
    auto q=std::clamp(std::lround(x*32768.),-32768l,32767l);
    s.pcm[i]=int16_t(q); s.input[i]=float(q)/32768.f;
}
Signal signal(std::size_t n,const char* name) {
    Signal s{std::vector<float>(n),std::vector<float>(n),std::vector<int16_t>(n)};
    std::uint32_t rng=0x1949u;
    const bool safe=std::strncmp(name,"safe",4)==0;
    const char* kind=safe?name+4:name;
    const bool weak=std::strncmp(kind,"weak",4)==0;
    double weak_db=weak?std::atof(kind+4):0;
    for(std::size_t i=0;i<n;++i) {
        double x=named_sample(i,n,name,kind,rng);
        if(weak) x=.5*std::cos(tau*13*i/n+.31)+.5*std::pow(10.,weak_db/20)*std::cos(tau*71*i/n+.77);
        if(safe) x*=.25;
        quantize_sample(s,i,x);
    }
    return s;
}
double snr(double signal_energy,double error_energy) {
    if(error_energy==0) return 300;
    if(signal_energy==0) return -300;
    return 10*std::log10(signal_energy/error_energy);
}
void accuracy(std::size_t n,const char* route,const char* name,const std::vector<float>& out,
              const std::vector<std::complex<double>>& ref) {
    double e=0,p=0,maxerr=0;
    for(std::size_t k=0;k<=n/2;++k) {
        std::complex<double> value(out[2*k],out[2*k+1]);
        double err=std::norm(value-ref[k]);
        e+=err; p+=std::norm(ref[k]); maxerr=std::max(maxerr,std::sqrt(err));
    }
    double weak_db=0,phase=0,reference_db=0;
    if(std::strstr(name,"weak")) {
        std::complex<double> v(out[142],out[143]);
        double r=std::abs(ref[71]);
        weak_db=std::abs(v)>0&&r>0?20*std::log10(std::abs(v)/r):-300;
        reference_db=20*std::log10(std::max(1e-20,r/std::abs(ref[13])));
        phase=std::remainder(std::arg(v)-std::arg(ref[71]),tau);
    }
    std::printf("accuracy N=%u route=%s signal=%s snr_db=%.6f max_raw_error=%.9g weak_error_db=%.6f weak_phase_error_rad=%.6f quantized_weak_dbc=%.6f\n",
        unsigned(n),route,name,snr(p,e),maxerr,weak_db,phase,reference_db);
}
class Gmf {
public:
    esp_gmf_fft_handle_t handle=nullptr;
    int16_t* data=nullptr;
    std::size_t n=0;
    bool hp=false;
    int log2n=0;
    bool prepare(std::size_t size,bool high_precision) {
        n=size; hp=high_precision; log2n=int(std::countr_zero(n/2));
        esp_gmf_fft_cfg_t cfg{int16_t(n),ESP_GMF_FFT_TYPE_REAL_Q15};
        if(esp_gmf_fft_init(&cfg,&handle)!=ESP_GMF_FFT_OK) return false;
        data=static_cast<int16_t*>(esp_gmf_fft_calloc_aligned(n+2,sizeof(int16_t),16));
        return data!=nullptr;
    }
    bool forward(const std::vector<int16_t>& pcm) {
        std::memcpy(data,pcm.data(),n*sizeof(int16_t));
        return (hp?esp_gmf_fft_forward_hp(handle,data):esp_gmf_fft_forward(handle,data))==ESP_GMF_FFT_OK;
    }
    bool inverse(const std::vector<int16_t>& spectrum) {
        std::memcpy(data,spectrum.data(),(n+2)*sizeof(int16_t));
        return (hp?esp_gmf_fft_inverse_hp(handle,data):esp_gmf_fft_inverse(handle,data))==ESP_GMF_FFT_OK;
    }
    void decode(std::vector<float>& out) const {
        int exp=hp?gmf_probe_exponent(handle):log2n-15;
        float scale=std::ldexp(1.f,exp);
        out[0]=float(data[0])*scale; out[1]=0;
        for(std::size_t k=1;k<n/2;++k) {out[2*k]=float(data[2*k])*scale; out[2*k+1]=float(data[2*k+1])*scale;}
        out[n]=float(data[1])*scale; out[n+1]=0;
    }
    ~Gmf() {esp_gmf_fft_free_aligned(data); esp_gmf_fft_deinit(&handle);}
};
void report_gmf_roundtrip(Gmf& g,const Signal& s,const char* route,const char* name,int exp) {
    const auto n=g.n;const bool hp=g.hp;
    double p=0,e=0,maxerr=0;
    for(std::size_t i=0;i<n;++i) {
        double restored=double(g.data[i])*(hp?1.:double(n)/4.);
        double err=restored-double(s.pcm[i]);
        p+=double(s.pcm[i])*s.pcm[i]; e+=err*err; maxerr=std::max(maxerr,std::abs(err));
    }
    std::printf("roundtrip N=%u route=%s signal=%s snr_db=%.6f max_q15_error=%.6f exponent=%d\n",unsigned(n),route,name,snr(p,e),maxerr,exp);
}
bool gmf_signal(Gmf& g,std::vector<float>& output,const char* route,const char* name) {
    const auto n=g.n;
    auto s=signal(n,name); auto ref=oracle(s.input);
    if(!g.forward(s.pcm)) return false;
    g.decode(output); accuracy(n,route,name,output,ref);
    int exp=gmf_probe_exponent(g.handle);
    std::vector<int16_t> spectrum(g.data,g.data+n+2);
    if(!g.inverse(spectrum)) return false;
    report_gmf_roundtrip(g,s,route,name,exp);
    if(!gmf_probe_guards_ok()) return false;
    yield();
    return true;
}
void report_gmf_timing(std::size_t n,const char* route,const std::array<double,2>& forward,
                       const std::array<double,2>& contract,const std::array<double,2>& inverse,
                       const std::array<double,2>& permutation,std::size_t allocations) {
    std::printf("timing N=%u route=%s forward_copy_us=%.3f forward_p95_us=%.3f float_contract_us=%.3f inverse_copy_us=%.3f inverse_p95_us=%.3f bitrev_only_us=%.3f hot_allocations=%u\n",
        unsigned(n),route,forward[0],forward[1],contract[0],inverse[0],inverse[1],permutation[0],unsigned(gmf_probe_allocations()-allocations));
}
bool finish_gmf_timing(Gmf& g,const Signal& s,const char* route,
        const std::array<double,2>& forward,const std::array<double,2>& contract,std::size_t allocations,bool& good) {
    const auto n=g.n;
    if(!g.forward(s.pcm)) return false;
    std::vector<int16_t> spectrum(g.data,g.data+n+2);
    auto inverse=timing([&]{good&=g.inverse(spectrum); sink=float(g.data[13]);});
    auto permutation=timing([&]{fft_radix2_bit_reverse_s16(g.data,int32_t(n/2),g.log2n); sink=float(g.data[13]);});
    report_gmf_timing(n,route,forward,contract,inverse,permutation,allocations);
    if(!good||gmf_probe_allocations()!=allocations) return false;
    return true;
}
bool time_gmf_route(Gmf& g,std::vector<float>& output,const char* route) {
    auto s=signal(g.n,"multitone");
    bool good=true;
    std::size_t allocations=gmf_probe_allocations();
    auto forward=timing([&]{good&=g.forward(s.pcm); sink=float(g.data[13]);});
    auto contract=timing([&]{good&=g.forward(s.pcm); g.decode(output); sink=output[13];});
    return finish_gmf_timing(g,s,route,forward,contract,allocations,good);
}
bool gmf_route(std::size_t n,bool hp) {
    const char* route=hp?"GMF_HP":"GMF_Q15";
    std::size_t free_before=free_heap();
    {
        Gmf g;
        if(!g.prepare(n,hp)) return false;
        std::size_t free_after=free_heap();
        std::vector<float> output(n+2);
        std::printf("memory N=%u route=%s plan_payload=%u io_payload=%u preserved_input=%u float_output=%u guards=%u heap_delta=%u\n",
            unsigned(n),route,unsigned(3*n+gmf_probe_plan_bytes()),unsigned(2*(n+2)),unsigned(2*n),unsigned(4*(n+2)),128u,
            unsigned(free_before-free_after));
        for(const char* name:{"zero","impulse","dc","nyquist","multitone","noise","weak-40","weak-60","weak-80","weak-90","safedc","safenoise","safeweak-40","safeweak-60","safeweak-80","safeweak-90"}) {
            if(!gmf_signal(g,output,route,name)) return false;
        }
        if(!time_gmf_route(g,output,route)) return false;
    }
    return gmf_probe_guards_ok()&&gmf_probe_live_bytes()==0&&platform_heap_ok();
}
bool float_signal(dsp_kernels::PreparedRfft& plan,std::vector<float>& out,std::size_t n,const char* route,const char* name) {
    auto s=signal(n,name); auto ref=oracle(s.input);
    if(!plan.process(s.input,out)) return false;
    accuracy(n,route,name,out,ref);
    double error=0;
    for(std::size_t k=0;k<=n/2;++k) {
        auto r=ref[k]; auto v=std::complex<double>(out[2*k],out[2*k+1]);
        error=std::max(error,std::abs(r-v));
        if(std::abs(r-v)>.0003+5e-6*std::abs(r)) return false;
    }
    yield();
    return true;
}
bool time_float_route(dsp_kernels::PreparedRfft& plan,std::vector<float>& out,std::size_t n,const char* route) {
    auto s=signal(n,"multitone"); bool good=true;
    auto t=timing([&]{good&=plan.process(s.input,out); sink=out[13];});
    std::printf("timing N=%u route=%s forward_copy_us=%.3f forward_p95_us=%.3f float_contract_us=%.3f inverse_copy_us=0 inverse_p95_us=0 hot_allocations=0\n",
        unsigned(n),route,t[0],t[1],t[0]);
    return good&&platform_heap_ok();
}
bool float_route(std::size_t n,bool radix4) {
    using namespace dsp_kernels;
    const char* route=radix4?"RFFT4":"RFFT2";
    PreparedRfft plan;
    if(!plan.prepare(n,radix4)) return false;
    std::vector<float> out(n+2);
    std::printf("memory N=%u route=%s plan_payload=%u io_payload=%u preserved_input=%u float_output=%u guards=0 heap_delta=0\n",
        unsigned(n),route,unsigned(plan.workspace_bytes()),0u,unsigned(4*n),unsigned(4*(n+2)));
    for(const char* name:{"zero","impulse","dc","nyquist","multitone","noise","weak-40","weak-60","weak-80","weak-90","safedc","safenoise","safeweak-40","safeweak-60","safeweak-80","safeweak-90"}) {
        if(!float_signal(plan,out,n,route,name)) return false;
    }
    return time_float_route(plan,out,n,route);
}
bool initialize_shared_tables() {
    // Initialize SDK shared tables once; payload is separate from per-plan data.
    {dsp_kernels::PreparedRfft r2,r4;if(!r2.prepare(4096)||!r4.prepare(4096,true)) return false;}
    auto estimate=dsp_kernels::estimate_memory(4096,0);
    if(!estimate) return false;
    std::printf("shared sdk_radix2=%u sdk_radix4=%u gmf_shared=0\n",unsigned(estimate->shared_radix2_tables_bytes),unsigned(estimate->shared_radix4_tables_bytes));
    return true;
}
bool validate_signal_oracle(std::size_t n) {
    auto s=signal(n,"multitone"); auto ref=oracle(s.input);
    return validate_oracle(s.input,ref);
}
bool run() {
    if(!initialize_shared_tables()) return false;
    std::size_t before=free_heap();
    for(std::size_t n:{512,1024,2048,4096}) {
        if(!validate_signal_oracle(n)) return false;
        if(!float_route(n,false)||!float_route(n,true)||!gmf_route(n,false)||!gmf_route(n,true)) return false;
    }
    std::printf("cleanup free_before=%u free_after=%u live_gmf=%u guards=%d\n",unsigned(before),unsigned(free_heap()),unsigned(gmf_probe_live_bytes()),gmf_probe_guards_ok());
    return before==free_heap()&&gmf_probe_live_bytes()==0&&gmf_probe_guards_ok();
}
#if defined(GMF_SPECTRUM_ONLY)
// Exact integer words avoid decimal serialization error. Export is outside the
// timing benchmark; raw FFT outputs and production arithmetic are unchanged.
void dump_input(const Signal& s,unsigned id) {
    for(std::size_t start=0;start<s.pcm.size();start+=16) {
        std::printf("P,%u,%u,",id,unsigned(start));
        for(std::size_t i=start;i<std::min(start+16,s.pcm.size());++i)
            std::printf("%04x",unsigned(std::uint16_t(s.pcm[i])));
        std::printf("\n");
        if(start%256==0) yield();
    }
}
void dump_output(const std::vector<float>& out,unsigned route,unsigned id) {
    for(std::size_t start=0;start<out.size();start+=16) {
        std::printf("D,%u,%u,%u,",route,id,unsigned(start));
        for(std::size_t i=start;i<std::min(start+16,out.size());++i)
            std::printf("%08x",unsigned(std::bit_cast<std::uint32_t>(out[i])));
        std::printf("\n");
        if(start%128==0) yield();
    }
}
bool run_spectra() {
    constexpr std::size_t n=4096;
    constexpr std::array names{"safeweak-60","safeweak-80","safenoise"};
    std::printf("spectra N=4096 signals=safeweak-60,safeweak-80,safenoise routes=RFFT2,RFFT4,GMF_Q15,GMF_HP serialization=hex_integer_words\n");
    {dsp_kernels::PreparedRfft r2,r4;if(!r2.prepare(n)||!r4.prepare(n,true)) return false;}
    const auto before=free_heap();
    {
        std::vector<float> out(n+2);
        for(unsigned id=0;id<names.size();++id) {
            auto s=signal(n,names[id]);
            {auto ref=oracle(s.input);if(!validate_oracle(s.input,ref)) return false;}
            dump_input(s,id);
            for(unsigned route=0;route<4;++route) {
                if(route<2) {
                    dsp_kernels::PreparedRfft plan;
                    if(!plan.prepare(n,route==1)||!plan.process(s.input,out)) return false;
                } else {
                    Gmf plan;
                    if(!plan.prepare(n,route==3)||!plan.forward(s.pcm)) return false;
                    plan.decode(out);
                }
                dump_output(out,route,id);
                if(!gmf_probe_guards_ok()||!platform_heap_ok()) return false;
            }
        }
    }
    std::printf("cleanup free_before=%u free_after=%u live_gmf=%u guards=%d\n",unsigned(before),unsigned(free_heap()),unsigned(gmf_probe_live_bytes()),gmf_probe_guards_ok());
    return before==free_heap()&&gmf_probe_live_bytes()==0&&gmf_probe_guards_ok();
}
#endif
bool entry() {
#if defined(ESP_PLATFORM)
    std::printf("gmf_comparison target=%s cpu_mhz=%d\n",CONFIG_IDF_TARGET,CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
#else
    std::printf("gmf_comparison target=windows compiler=%s\n",__VERSION__);
#endif
#if defined(GMF_SPECTRUM_ONLY)
    std::printf("scope pinned=255fb531ddc97ff5e1e113dad97c88f70d6297be spectra_only=1 timing=excluded no_gain_fit=1\n");
    bool good=run_spectra();
#else
    std::printf("scope pinned=255fb531ddc97ff5e1e113dad97c88f70d6297be warmups=24 batches=96 calls=8 window_and_quantization=excluded reference=unnormalized_double_dft no_gain_fit=1\n");
    bool good=run();
#endif
    std::printf("fft_benchmark result=%s\n",good?"PASS":"FAIL");
    return good;
}
}
#if defined(ESP_PLATFORM)
extern "C" void app_main() { entry(); }
#else
int main() { return entry()?0:1; }
#endif
