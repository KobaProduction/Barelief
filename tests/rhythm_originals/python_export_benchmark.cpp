#include "python_export_benchmark.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <new>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <malloc.h>
#endif

// Declare this translation unit's replacement sized overloads before the tracker
// calls them; the standard library may only expose them with sized deallocation.
void operator delete(void*,std::size_t) noexcept;
void operator delete(void*,std::size_t,std::align_val_t) noexcept;

namespace originals::benchmark {
namespace {
constexpr std::size_t slots=65536;
struct Entry {void* address=nullptr;std::size_t bytes=0;std::uint64_t epoch=0;};
struct Cold {std::size_t bound=0;Allocations allocations;bool ok=false;};
std::array<Entry,slots> entries;
thread_local Allocations* active=nullptr;
thread_local std::uint64_t epoch=0;
thread_local Cold cold;
void* tombstone() noexcept {return reinterpret_cast<void*>(std::uintptr_t{1});}
std::size_t insertion_slot(std::size_t index,std::size_t available,bool insert) noexcept {
    if(!insert)return slots;
    return available==slots?index:available;
}

std::size_t slot(void* address,bool insert) noexcept {
    auto index=(reinterpret_cast<std::uintptr_t>(address)>>4)&(slots-1);auto available=slots;
    for(std::size_t count=0;count<slots;++count){
        const auto key=entries[index].address;
        if(key==address)return index;
        if(!key)return insertion_slot(index,available,insert);
        if(key==tombstone() && available==slots)available=index;
        index=(index+1)&(slots-1);
    }
    return insert?available:slots;
}
void allocated(void* address,std::size_t bytes) noexcept {
    if(!active)return;
    ++active->calls;const auto index=slot(address,true);
    if(index==slots || bytes>std::numeric_limits<std::size_t>::max()-active->live){active->overflow=true;return;}
    entries[index]={address,bytes,active->epoch};active->live+=bytes;
    active->peak=std::max(active->peak,active->live);
}
void released(void* address) noexcept {
    if(!active || !address)return;
    const auto index=slot(address,false);if(index==slots)return;
    const auto entry=entries[index];entries[index].address=tombstone();
    if(entry.epoch==active->epoch)active->live-=entry.bytes;
}
#if defined(_WIN32)
std::uint64_t ticks(const FILETIME& value) noexcept {
    return (std::uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;
}
bool read_cycles(std::uint64_t& output) noexcept {
    using Query=BOOL(WINAPI*)(HANDLE,PULONG64);
    static const auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"QueryThreadCycleTime"));
    ULONG64 value=0;if(!query || !query(GetCurrentThread(),&value))return false;
    output=value;return true;
}
#endif
bool valid(const Warmup& warmup,const Samples& samples,std::size_t workspace) noexcept {
    return cold.ok && workspace<=cold.bound && warmup.ok && samples.ok && samples.count==replays &&
        !cold.allocations.overflow && !warmup.allocations.overflow && !samples.allocations.overflow &&
        !warmup.allocations.calls && !samples.allocations.calls;
}
bool tracker_self_check() noexcept {
    AllocationWatch watch;
    auto* ordinary=::operator new(32,std::nothrow);
    auto* aligned=::operator new(64,std::align_val_t{64},std::nothrow);
    auto* array=::operator new[](16,std::nothrow);
    const bool pointers=ordinary && aligned && array && reinterpret_cast<std::uintptr_t>(aligned)%64==0;
    ::operator delete(ordinary,std::size_t{32});
    ::operator delete(aligned,std::size_t{64},std::align_val_t{64});
    ::operator delete[](array,std::nothrow);
    const auto result=watch.finish();
    return pointers && !result.overflow && result.calls==3 && result.peak==112 && result.live==0;
}
bool nested_tracker_rejected() noexcept {
    AllocationWatch outer;Allocations nested;
    {AllocationWatch inner;nested=inner.finish();}
    const auto parent=outer.finish();return nested.overflow && parent.overflow;
}
void write_memory(std::ostream& output,std::size_t workspace,const Warmup& warmup,const Samples& samples) {
    output<<"\"memory\":{\"queried_cold_peak_bound_bytes\":"<<cold.bound<<",\"owned_workspace_bytes\":"<<workspace
        <<",\"cold_cpp_requested_payload_peak_bytes\":"<<cold.allocations.peak
        <<",\"cold_cpp_live_after_prepare_bytes\":"<<cold.allocations.live
        <<",\"cold_cpp_allocation_calls\":"<<cold.allocations.calls
        <<",\"warmup_cpp_allocation_calls\":"<<warmup.allocations.calls
        <<",\"hot_cpp_allocation_calls\":"<<samples.allocations.calls
        <<",\"tracking_table_storage_bytes\":"<<sizeof(entries)
        <<",\"allocation_tracker_self_check_passed\":"<<(tracker_ready()?"true":"false")
        <<",\"tracking_complete\":"<<(!cold.allocations.overflow && !warmup.allocations.overflow && !samples.allocations.overflow?"true":"false")
        <<",\"all_heap_measured\":false,\"bound_validation\":\"owned workspace only; partial CPP peak does not validate all-heap cold peak\""
        <<",\"coverage\":\"ordinary/aligned/sized/nothrow global C++ new/delete on measurement thread; C/PocketFFT malloc not intercepted; requested payload excludes allocator overhead\"},";
}
void write_cpu(std::ostream& output,const Samples& samples) {
    const bool ticks_valid=samples.first.ticks_valid && samples.last.ticks_valid && samples.last.ticks>=samples.first.ticks;
    const bool cycles_valid=samples.first.cycles_valid && samples.last.cycles_valid && samples.last.cycles>=samples.first.cycles;
    const auto elapsed=ticks_valid?samples.last.ticks-samples.first.ticks:0;
    const auto cycles=cycles_valid?samples.last.cycles-samples.first.cycles:0;
    output<<"\"cpu\":{\"aggregate_thread_cpu_seconds\":";
    if(ticks_valid)output<<double(elapsed)/1e7;else output<<"null";
    output<<",\"cpu_seconds_usable\":"<<(ticks_valid && elapsed?"true":"false")<<",\"aggregate_thread_cpu_cycles\":";
    if(cycles_valid)output<<cycles;else output<<"null";
    output<<",\"cycles_per_replay\":";
    if(cycles_valid && samples.count)output<<double(cycles)/double(samples.count);else output<<"null";
    output<<",\"scope\":\"aggregate measurement loop, including timer/sample bookkeeping; OS thread CPU accounting may be coarser than one replay\"},";
}
void write_raw_times(std::ostream& output,const Samples& samples) {
    output<<",\"raw\":[";
    for(std::size_t i=0;i<samples.count;++i){if(i)output<<',';output<<samples.microseconds[i];}
    output<<"]";
}

void write_times(std::ostream& output,const Samples& samples) {
    auto sorted=samples.microseconds;std::sort(sorted.begin(),sorted.begin()+std::ptrdiff_t(samples.count));
    output<<"\"latency\":{\"unit\":\"microseconds\",\"scope\":\"one complete prepared process with in-memory input/output callbacks; excludes prepare/import/subprocess/file I/O\",\"p50\":";
    if(samples.count)output<<sorted[samples.count/2];else output<<"null";
    output<<",\"p95_nearest_rank\":";
    if(samples.count)output<<sorted[(95*samples.count+99)/100-1];else output<<"null";
    write_raw_times(output,samples);output<<"}";
}
}
AllocationWatch::AllocationWatch() noexcept:previous_(active) {
    values_.epoch=++epoch;values_.overflow=bool(active);
    if(previous_)previous_->overflow=true;
    active=&values_;
}
AllocationWatch::~AllocationWatch() {if(!finished_)(void)finish();}
Allocations AllocationWatch::finish() noexcept {
    if(!finished_){values_.overflow=values_.overflow || active!=&values_;active=previous_;finished_=true;}
    return values_;
}
bool enabled() noexcept {
    static const bool value=[](){const auto* option=std::getenv("BARELIEF_DSP_BENCHMARK");return option && option[0]=='1' && !option[1];}();
    return value;
}
bool tracker_ready() noexcept {static const bool passed=tracker_self_check() && nested_tracker_rejected();return passed;}
CpuReading cpu_reading() noexcept {
    CpuReading result;
#if defined(_WIN32)
    FILETIME created{},exited{},kernel{},user{};
    result.ticks_valid=GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)!=0;
    if(result.ticks_valid)result.ticks=ticks(kernel)+ticks(user);
    result.cycles_valid=read_cycles(result.cycles);
#endif
    return result;
}
void cold_finished(std::size_t bound,Allocations values,bool ok) noexcept {cold={bound,values,ok};}
bool write_result(const std::filesystem::path& prefix,std::size_t workspace,const Warmup& warmup,const Samples& samples) {
    std::ofstream output(prefix.string()+".benchmark.json");const bool passed=valid(warmup,samples,workspace);
    output<<std::setprecision(17)<<"{\"format\":\"barelief-prepared-dsp-benchmark-v1\",\"passed\":"<<(passed?"true":"false")
        <<",\"warmups\":"<<warmups<<",\"measured_replays\":"<<samples.count<<',';
    write_memory(output,workspace,warmup,samples);write_cpu(output,samples);write_times(output,samples);
    output<<"}\n";return passed && bool(output);
}
void* allocate(std::size_t bytes) {
    if(auto* address=std::malloc(bytes?bytes:1)){allocated(address,bytes);return address;}
    throw std::bad_alloc();
}
void deallocate(void* address) noexcept {released(address);std::free(address);}
void* allocate_aligned(std::size_t bytes,std::size_t alignment) {
    void* address=nullptr;
#if defined(_WIN32)
    address=_aligned_malloc(bytes?bytes:1,alignment);
#else
    if(bytes<=std::numeric_limits<std::size_t>::max()-alignment)
        address=std::aligned_alloc(alignment,((bytes?bytes:1)+alignment-1)/alignment*alignment);
#endif
    if(!address)throw std::bad_alloc();
    allocated(address,bytes);return address;
}
void deallocate_aligned(void* address) noexcept {
    released(address);
#if defined(_WIN32)
    _aligned_free(address);
#else
    std::free(address);
#endif
}
}
void* operator new(std::size_t bytes) {return originals::benchmark::allocate(bytes);}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void* address) noexcept {originals::benchmark::deallocate(address);}
void operator delete[](void* address) noexcept {::operator delete(address);}
void operator delete(void* address,std::size_t) noexcept {::operator delete(address);}
void operator delete[](void* address,std::size_t) noexcept {::operator delete(address);}
void* operator new(std::size_t bytes,std::align_val_t alignment) {return originals::benchmark::allocate_aligned(bytes,std::size_t(alignment));}
void* operator new[](std::size_t bytes,std::align_val_t alignment) {return ::operator new(bytes,alignment);}
void operator delete(void* address,std::align_val_t) noexcept {originals::benchmark::deallocate_aligned(address);}
void operator delete[](void* address,std::align_val_t alignment) noexcept {::operator delete(address,alignment);}
void operator delete(void* address,std::size_t,std::align_val_t alignment) noexcept {::operator delete(address,alignment);}
void operator delete[](void* address,std::size_t,std::align_val_t alignment) noexcept {::operator delete(address,alignment);}
void* operator new(std::size_t bytes,const std::nothrow_t&) noexcept {try{return ::operator new(bytes);}catch(...){return nullptr;}}
void* operator new[](std::size_t bytes,const std::nothrow_t&) noexcept {try{return ::operator new[](bytes);}catch(...){return nullptr;}}
void operator delete(void* address,const std::nothrow_t&) noexcept {::operator delete(address);}
void operator delete[](void* address,const std::nothrow_t&) noexcept {::operator delete[](address);}
void* operator new(std::size_t bytes,std::align_val_t alignment,const std::nothrow_t&) noexcept {try{return ::operator new(bytes,alignment);}catch(...){return nullptr;}}
void* operator new[](std::size_t bytes,std::align_val_t alignment,const std::nothrow_t&) noexcept {try{return ::operator new[](bytes,alignment);}catch(...){return nullptr;}}
void operator delete(void* address,std::align_val_t alignment,const std::nothrow_t&) noexcept {::operator delete(address,alignment);}
void operator delete[](void* address,std::align_val_t alignment,const std::nothrow_t&) noexcept {::operator delete[](address,alignment);}
