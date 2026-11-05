#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace originals::benchmark {
inline constexpr std::size_t warmups=3,replays=64;
struct Allocations {
    std::size_t calls=0,live=0,peak=0;
    std::uint64_t epoch=0;
    bool overflow=false;
};
class AllocationWatch {
public:
    AllocationWatch() noexcept;
    ~AllocationWatch();
    Allocations finish() noexcept;
private:
    Allocations values_;
    Allocations* previous_=nullptr;
    bool finished_=false;
};
struct CpuReading {
    std::uint64_t ticks=0,cycles=0;
    bool ticks_valid=false,cycles_valid=false;
};
struct Warmup {
    Allocations allocations;
    bool ok=false;
};
struct Samples {
    std::array<double,replays> microseconds{};
    std::size_t count=0;
    Allocations allocations;
    CpuReading first,last;
    bool ok=false;
};
bool enabled() noexcept;
bool tracker_ready() noexcept;
CpuReading cpu_reading() noexcept;
void cold_finished(std::size_t bound,Allocations,bool ok) noexcept;
bool write_result(const std::filesystem::path&,std::size_t workspace,const Warmup&,const Samples&);

template<typename Prepare> bool measured_prepare(std::size_t bound,Prepare&& prepare) {
    if(!enabled())return prepare();
    if(!tracker_ready())return false;
    AllocationWatch watch;
    const bool ok=prepare();cold_finished(bound,watch.finish(),ok);return ok;
}

template<typename Query,typename Prepare> bool prepare(Query&& query,Prepare&& prepare) {
    std::size_t bound=0;
    if(enabled() && !query(bound))return false;
    return measured_prepare(bound,prepare);
}
template<typename Process> Warmup warm(Process& process) {
    AllocationWatch watch;bool ok=true;
    for(std::size_t i=0;i<warmups;++i)if(!process()){ok=false;break;}
    return {watch.finish(),ok};
}
template<typename Process> Samples measure(Process& process) {
    Samples samples;AllocationWatch watch;samples.first=cpu_reading();
    for(std::size_t i=0;i<replays;++i){
        const auto first=std::chrono::steady_clock::now();samples.ok=process();
        samples.microseconds[i]=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-first).count();
        ++samples.count;if(!samples.ok)break;
    }
    samples.last=cpu_reading();samples.allocations=watch.finish();return samples;
}
template<typename Process> bool measured_process(const std::filesystem::path& prefix,std::size_t workspace,Process&& process) {
    if(!enabled())return process();
    const auto warmup=warm(process);
    return write_result(prefix,workspace,warmup,warmup.ok?measure(process):Samples{});
}
}
