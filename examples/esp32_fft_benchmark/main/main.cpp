#include <dsp_kernels/dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(DSP_FFT_PROFILE)
#include "fft_profile.h"
#include "esp_cpu.h"
namespace dsp_kernels::detail {
std::array<std::uint64_t, std::size_t(FftStage::Count)> stage_cycles{};
std::uint32_t fft_profile_clock() noexcept { return esp_cpu_get_cycle_count(); }
void fft_profile_record(FftStage stage, std::uint32_t cycles) noexcept { stage_cycles[std::size_t(stage)] += cycles; }
}
#endif
#include "correctness.h"
#include "memory_checks.h"
namespace {
constexpr std::size_t batches = 96, batch = 8;
// One plan at a time; ordinary caller buffers need no SIMD alignment.
template<class Plan> bool measure(Plan& plan, const char* name,
    std::span<const float> input, std::span<float> output, std::span<const float> reference) {
    if constexpr(requires { plan.split_implementation(); })
        std::printf("fft_route N=%u route=%s split=%s\n",unsigned(input.size()),name,plan.split_implementation());
    if (!plan.process(input, output)) return false;
    double error = 0;
    for (std::size_t i=0;i<output.size();++i) {
        if(!std::isfinite(output[i])) return false;
        const auto difference = std::abs(double(output[i])-reference[i]);
        error = std::max(error,difference);
        if (difference > .0003+5e-6*std::abs(double(reference[i]))) return false;
    }
    std::array<double,batches> times{};
    for (unsigned warm=0;warm<24;++warm) if(!plan.process(input,output))return false;
#if defined(DSP_FFT_PROFILE)
    dsp_kernels::detail::stage_cycles.fill(0);
#endif
    for (auto& us:times) {
        const auto begin=esp_timer_get_time();
        for(unsigned i=0;i<batch;++i)if(!plan.process(input,output))return false;
        us=double(esp_timer_get_time()-begin)/batch;
        vTaskDelay(1); // Bounded benchmark yields to platform tasks.
    }
    std::sort(times.begin(),times.end());
    std::printf("raw_fft target=%s N=%u route=%s p50_us=%.3f p95_batch_mean_us=%.3f workspace=%u max_abs_difference=%.9g free_heap=%u minimum_free_heap=%u\n",
        CONFIG_IDF_TARGET,unsigned(input.size()),name,times[batches/2],times[batches*95/100],unsigned(plan.workspace_bytes()),error,
        unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
#if defined(DSP_FFT_PROFILE)
    constexpr std::array stages{"input","pack","kernel","permutation","mixed","split","validate","publish"};
    for (std::size_t i=0;i<stages.size();++i)
        std::printf("fft_stage N=%u route=%s stage=%s mean_us=%.3f\n",unsigned(input.size()),name,stages[i],double(dsp_kernels::detail::stage_cycles[i])/(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ*batches*batch));
#endif
    return true;
}
bool run_size(std::size_t n) {
    using namespace dsp_kernels;
    std::vector<float> input(n),coefficients(n),reference(n+2),output(n+2);
    window(coefficients,WindowFunction::Hann);
    constexpr std::array frequencies{3.,11.25,23.,41.5,67.,89.75,111.};
    for(std::size_t i=0;i<n;++i) {
        double x=.17+(i%2?-.11:.11);
        for(std::size_t tone=0;tone<frequencies.size();++tone)
            x+=.25/double(tone+1)*std::cos(6.283185307179586*frequencies[tone]*double(i)/n+.13*tone);
        input[i]=float(x)*coefficients[i];
    }
    {FFT oracle;if(!oracle.prepare(n)||!oracle.process(input,reference))return false;}
    for(bool radix4:{false,true}) {
        // ESP-DSP's full radix-4 transform accepts only powers of four.
        if(!radix4 || (n & 0x55555555u)) {
            FFT full;
            if(!full.prepare(n,radix4)||!measure(full,radix4?"CFFT4":"CFFT2",input,output,reference))return false;
        } else {
            FFT unsupported;
            if(unsupported.prepare(n,true)) return false;
            std::printf("raw_fft target=%s N=%u route=CFFT4 status=unsupported reason=requires_power_of_four\n",CONFIG_IDF_TARGET,unsigned(n));
        }
        {PreparedRfft real;if(!real.prepare(n,radix4)||!measure(real,radix4?"RFFT4":"RFFT2",input,output,reference))return false;}
        {PreparedRfft real;if(!real.prepare(n,radix4,PreparedRfft::Split::Scalar)||!measure(real,radix4?"RFFT4-scalar":"RFFT2-scalar",input,output,reference))return false;}
    }
    return true;
}
bool run() {
    if(!verify_memory()) return false;
    if(!verify_math()) return false;
#if defined(FFT_MEMORY_ONLY)
    return true;
#else
    for(std::size_t n:{512,1024,2048,4096}) if(!run_size(n)) return false;
    return true;
#endif
}
}
extern "C" void app_main() {
#if defined(CONFIG_DSP_OPTIMIZED) && CONFIG_DSP_OPTIMIZED
    constexpr int optimized = 1;
#else
    constexpr int optimized = 0;
#endif
#if defined(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ)
    constexpr int cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
#else
    constexpr int cpu_mhz = 0;
#endif
    std::printf("fft_benchmark target=%s cpu_config_mhz=%d sdk_optimized=%d sizes=512,1024,2048,4096 warmups=24 batches=96 batch_size=8\n",
        CONFIG_IDF_TARGET,cpu_mhz,optimized);
    std::printf("fft_benchmark result=%s\n",run()?"PASS":"FAIL");
}
