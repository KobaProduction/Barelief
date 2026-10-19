#include "dot_probe.h"
extern "C" void app_main(){
    const auto ok=dot_probe::run();
    std::printf("RESAMPLER_BENCH_%s scope=dot_probe cpu_mhz=%lu\n",ok?"PASS":"FAIL",(unsigned long)esp_rom_get_cpu_ticks_per_us());
    std::fflush(stdout);
    while(true)vTaskDelay(pdMS_TO_TICKS(1000));
}
