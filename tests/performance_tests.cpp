#include "core/performance.h"
#include <thread>
#include <vector>
#include <cstdio>
int main() {
    using namespace core::perf;
    record(Stage::FFT,1,1000);
    if(counters[static_cast<size_t>(Stage::FFT)].count!=0) return 1;
    enabled=true;
    std::vector<std::thread> workers;
    for(int thread=0;thread<4;++thread) workers.emplace_back([] {
        for(int i=0;i<10000;++i) record(Stage::FFT,1,1000);
    });
    for(auto &worker:workers) worker.join();enabled=false;
    const auto &counter=counters[static_cast<size_t>(Stage::FFT)];
    if(counter.count!=40000 || counter.total!=40000000 || counter.maximum!=1000 || event_count!=40000) return 2;
    for(const auto &event:events) if(!event.ready || event.duration!=1000 || event.stage!=Stage::FFT) return 3;
    Counter distribution;
    if (percentile_upper_ns(distribution, 95) != 0) return 4;
    distribution.count = 100; distribution.maximum = 1000000000;
    distribution.histogram[0] = 97; distribution.histogram[histogram_bins - 1] = 3;
    if (percentile_upper_ns(distribution, 50) != bin_ns ||
        percentile_upper_ns(distribution, 95) != bin_ns ||
        percentile_upper_ns(distribution, 99) != 1000000000 ||
        percentile_upper_ns(distribution, 100) != 1000000000) return 5;
    std::puts("Telemetry is disabled by default, concurrent counters are correct and trace storage remains bounded");
    return 0;
}
