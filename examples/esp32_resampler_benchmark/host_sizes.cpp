#include "common/packet_benchmark.h"
#include <speex_backend.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
extern "C" std::size_t qtvis_speex_allocation_calls();
namespace {std::atomic<std::size_t> allocations{0};}
void* operator new(std::size_t bytes) {++allocations;if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
int main() {
#ifdef _WIN32
    // Benchmark-local affinity only; avoids migration on hybrid Windows CPUs.
    if(!SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1))){std::fprintf(stderr,"FAIL thread affinity\n");return 1;}
#endif
    const auto& factory=desktop_resampling::speex_backend_factory();
    std::printf("{\"check\":\"platform\",\"platform\":\"windows\",\"pointer_bits\":%zu,\"logical_cpu\":0,\"thread_affinity\":true,\"individual_latency\":false,\"minimum_batch_target_us\":2000}\n",sizeof(void*)*8);
    const resampler_packet_benchmark::Hooks hooks{
        []()->std::uint64_t{return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();},1000.,false,
        [](){std::this_thread::yield();},[]()->std::size_t{return 0;},[](){return true;},
        [](){return allocations.load()+qtvis_speex_allocation_calls();},2000};
    const auto begin=std::chrono::steady_clock::now();const bool ok=resampler_packet_benchmark::run(hooks,factory);
    std::printf("RESAMPLER_BENCH_%s scope=packet_sweep elapsed_ms=%lld\n",ok?"PASS":"FAIL",
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count());
    return ok?0:1;
}
