#include "../common/packet_benchmark.h"
#include <speex_backend.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
extern "C" std::size_t qtvis_speex_allocation_calls();
extern "C" void app_main() {
    const auto& factory=desktop_resampling::speex_backend_factory();
    std::printf("RESAMPLER_BENCH_WARMUP %.6f\n",1.234567);
    const auto heap_before=heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const auto begin=esp_timer_get_time();
    std::printf("{\"check\":\"platform\",\"platform\":\"%s\",\"cpu_mhz\":%lu,\"free_heap\":%zu,\"pointer_bits\":%zu,\"individual_latency\":true}\n",
        CONFIG_IDF_TARGET,(unsigned long)esp_rom_get_cpu_ticks_per_us(),heap_before,sizeof(void*)*8);
    const resampler_packet_benchmark::Hooks hooks{
        []()->std::uint64_t{return esp_cpu_get_cycle_count();},double(esp_rom_get_cpu_ticks_per_us()),true,
        [](){vTaskDelay(1);},[](){return heap_caps_get_free_size(MALLOC_CAP_8BIT);},
        [](){return heap_caps_check_integrity_all(true);},qtvis_speex_allocation_calls,0};
    bool ok=resampler_packet_benchmark::run(hooks,factory);
    const auto heap_after=heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ok=ok && heap_before==heap_after && heap_caps_check_integrity_all(true);
    std::printf("RESAMPLER_BENCH_%s scope=packet_sweep free_before=%zu free_after=%zu minimum_free=%zu elapsed_ms=%lld\n",
        ok?"PASS":"FAIL",heap_before,heap_after,heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),(long long)((esp_timer_get_time()-begin)/1000));
    std::fflush(stdout);while(true)vTaskDelay(pdMS_TO_TICKS(1000));
}
