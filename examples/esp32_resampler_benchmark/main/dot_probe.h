#pragma once
#include <dsp_kernels/pcm_resampler.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <esp_cpu.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace dot_probe {
using Function=float(*)(const float*,const float*,const float*,std::size_t,float);
#define PROBE_NOINLINE __attribute__((noinline))
// Frozen production arithmetic before the second optimization pass.
PROBE_NOINLINE inline float old4(const float* x,const float* a,const float* b,std::size_t n,float blend){
    float a0=0,a1=0,a2=0,a3=0;std::size_t k=0;
    if(blend==0){for(;k+4<=n;k+=4){a0+=x[k]*a[k];a1+=x[k+1]*a[k+1];a2+=x[k+2]*a[k+2];a3+=x[k+3]*a[k+3];}
        float sum=(a0+a1)+(a2+a3);for(;k<n;++k)sum+=x[k]*a[k];return sum;}
    float b0=0,b1=0,b2=0,b3=0;
    for(;k+4<=n;k+=4){a0+=x[k]*a[k];a1+=x[k+1]*a[k+1];a2+=x[k+2]*a[k+2];a3+=x[k+3]*a[k+3];
        b0+=x[k]*b[k];b1+=x[k+1]*b[k+1];b2+=x[k+2]*b[k+2];b3+=x[k+3]*b[k+3];}
    float first=(a0+a1)+(a2+a3),second=(b0+b1)+(b2+b3);
    for(;k<n;++k){first+=x[k]*a[k];second+=x[k]*b[k];}return first+blend*(second-first);
}
PROBE_NOINLINE inline float pair2(const float* x,const float* a,const float* b,std::size_t n,float blend){
    float a0=0,a1=0,b0=0,b1=0;std::size_t k=0;
    if(blend==0){for(;k+2<=n;k+=2){a0+=x[k]*a[k];a1+=x[k+1]*a[k+1];}
        float sum=a0+a1;for(;k<n;++k)sum+=x[k]*a[k];return sum;}
    for(;k+2<=n;k+=2){const auto x0=x[k],x1=x[k+1];a0+=x0*a[k];b0+=x0*b[k];a1+=x1*a[k+1];b1+=x1*b[k+1];}
    float first=a0+a1,second=b0+b1;for(;k<n;++k){first+=x[k]*a[k];second+=x[k]*b[k];}
    return first+blend*(second-first);
}
PROBE_NOINLINE inline float blocks4(const float* x,const float* a,const float* b,std::size_t n,float blend){
    float a0=0,a1=0,a2=0,a3=0;std::size_t k=0;
    if(blend==0){for(auto blocks=n/4;blocks;--blocks,k+=4){a0+=x[k]*a[k];a1+=x[k+1]*a[k+1];a2+=x[k+2]*a[k+2];a3+=x[k+3]*a[k+3];}
        float sum=(a0+a1)+(a2+a3);for(;k<n;++k)sum+=x[k]*a[k];return sum;}
    float b0=0,b1=0,b2=0,b3=0;
    for(auto blocks=n/4;blocks;--blocks,k+=4){a0+=x[k]*a[k];a1+=x[k+1]*a[k+1];a2+=x[k+2]*a[k+2];a3+=x[k+3]*a[k+3];
        b0+=x[k]*b[k];b1+=x[k+1]*b[k+1];b2+=x[k+2]*b[k+2];b3+=x[k+3]*b[k+3];}
    float first=(a0+a1)+(a2+a3),second=(b0+b1)+(b2+b3);for(;k<n;++k){first+=x[k]*a[k];second+=x[k]*b[k];}
    return first+blend*(second-first);
}
PROBE_NOINLINE inline float fused4(const float* x,const float* a,const float* b,std::size_t n,float blend){
    if(blend==0)return old4(x,a,b,n,blend);
    float s0=0,s1=0,s2=0,s3=0;std::size_t k=0;
    for(;k+4<=n;k+=4){s0+=x[k]*(a[k]+blend*(b[k]-a[k]));s1+=x[k+1]*(a[k+1]+blend*(b[k+1]-a[k+1]));
        s2+=x[k+2]*(a[k+2]+blend*(b[k+2]-a[k+2]));s3+=x[k+3]*(a[k+3]+blend*(b[k+3]-a[k+3]));}
    float sum=(s0+s1)+(s2+s3);for(;k<n;++k)sum+=x[k]*(a[k]+blend*(b[k]-a[k]));return sum;
}
PROBE_NOINLINE inline float scalar(const float* x,const float* a,const float* b,std::size_t n,float blend){
    float first=0,second=0;if(blend==0){for(std::size_t k=0;k<n;++k)first+=x[k]*a[k];return first;}
    for(std::size_t k=0;k<n;++k){first+=x[k]*a[k];second+=x[k]*b[k];}return first+blend*(second-first);
}
template<unsigned Unroll> PROBE_NOINLINE float unrolled4(const float* x,const float* a,const float* b,std::size_t n,float blend){
    float a0=0,a1=0,a2=0,a3=0,b0=0,b1=0,b2=0,b3=0;std::size_t k=0;
    if(blend==0){
        for(;k+Unroll<=n;k+=Unroll){
            #pragma GCC unroll 4
            for(unsigned j=0;j<Unroll;j+=4){const auto p=k+j;a0+=x[p]*a[p];a1+=x[p+1]*a[p+1];a2+=x[p+2]*a[p+2];a3+=x[p+3]*a[p+3];}
        }
        for(;k+4<=n;k+=4){a0+=x[k]*a[k];a1+=x[k+1]*a[k+1];a2+=x[k+2]*a[k+2];a3+=x[k+3]*a[k+3];}
        float sum=(a0+a1)+(a2+a3);for(;k<n;++k)sum+=x[k]*a[k];return sum;
    }
    for(;k+Unroll<=n;k+=Unroll){
        #pragma GCC unroll 4
        for(unsigned j=0;j<Unroll;j+=4){const auto p=k+j;
            a0+=x[p]*a[p];a1+=x[p+1]*a[p+1];a2+=x[p+2]*a[p+2];a3+=x[p+3]*a[p+3];
            b0+=x[p]*b[p];b1+=x[p+1]*b[p+1];b2+=x[p+2]*b[p+2];b3+=x[p+3]*b[p+3];}
    }
    for(;k+4<=n;k+=4){a0+=x[k]*a[k];a1+=x[k+1]*a[k+1];a2+=x[k+2]*a[k+2];a3+=x[k+3]*a[k+3];
        b0+=x[k]*b[k];b1+=x[k+1]*b[k+1];b2+=x[k+2]*b[k+2];b3+=x[k+3]*b[k+3];}
    float first=(a0+a1)+(a2+a3),second=(b0+b1)+(b2+b3);for(;k<n;++k){first+=x[k]*a[k];second+=x[k]*b[k];}
    return first+blend*(second-first);
}
inline bool run(){
    volatile float sink=0;bool ok=true;
    for(const auto quality:{dsp_kernels::PcmResampler::Quality::Full,dsp_kernels::PcmResampler::Quality::Compact}){
        dsp_kernels::PcmResampler filter;if(!filter.prepare(48000,32768,1,quality))return false;
        const auto g=filter.filter_geometry();const auto w=filter.coefficients();std::vector<float> x(g.taps);
        for(std::size_t k=0;k<x.size();++k)x[k]=.3f*std::sin(.1f*k)+.2f*std::cos(.07f*k);
        const float* a=w.data()+8*g.taps;const float* b=a+g.taps;
        for(const float blend:{0.f,.37f})for(const auto& entry:{std::pair{"old4",Function(old4)},std::pair{"blocks4",Function(blocks4)},std::pair{"pair2",Function(pair2)},
                                                            std::pair{"unroll8",Function(unrolled4<8>)},std::pair{"unroll16",Function(unrolled4<16>)},
                                                            std::pair{"fused4",Function(fused4)},std::pair{"scalar",Function(scalar)}}){
            double first=0,second=0;for(std::size_t k=0;k<g.taps;++k){first+=double(x[k])*a[k];second+=double(x[k])*b[k];}
            const auto error=std::abs(double(entry.second(x.data(),a,b,g.taps,blend))-(first+blend*(second-first)));ok=ok&&error<2e-6;
            std::array<std::uint32_t,33> times{};constexpr unsigned batch=64;
            for(auto& t:times){vTaskDelay(1);const auto begin=esp_cpu_get_cycle_count();
                for(unsigned i=0;i<batch;++i){asm volatile("" ::: "memory");sink=entry.second(x.data(),a,b,g.taps,blend);}
                t=esp_cpu_get_cycle_count()-begin;}
            std::sort(times.begin(),times.end());
            std::printf("{\"check\":\"dot_probe\",\"variant\":\"%s\",\"taps\":%zu,\"blend\":%.3f,\"max_abs\":%.9g,\"p50_cycles_per_dot\":%.3f,\"p95_cycles_per_dot\":%.3f}\n",
                entry.first,g.taps,double(blend),error,double(times[16])/batch,double(times[31])/batch);std::fflush(stdout);
        }
    }
    (void)sink;return ok;
}
#undef PROBE_NOINLINE
}
