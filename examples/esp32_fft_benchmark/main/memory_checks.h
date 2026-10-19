#pragma once
#include <atomic>
#include <cstring>
#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
namespace {
bool verify_vendor_memory() {
    using dsp_kernels::detail::FftStorage;
    // No prepared plan may live across direct SDK init/deinit.
    FftStorage table(4*4096+8,-321.f),data(2*4096+8,-321.f);
    if(table.size()!=4*4096+8 || data.size()!=2*4096+8)return false;
    for(unsigned cycle=0;cycle<4;++cycle) for(int n:{16,64,256,1024,4096}) {
        std::fill(table.begin(),table.end(),-321.f);
        std::fill(data.begin(),data.end(),-321.f);
        float* const twiddles=table.data()+4;
        float* const raw=data.data()+4;
        std::fill_n(raw,2*n,0.f);raw[0]=1.f;
        if(dsps_fft4r_init_fc32(twiddles,n)!=ESP_OK) return false;
        const bool ok=dsps_fft4r_fc32(raw,n)==ESP_OK && dsps_bit_rev4r_fc32(raw,n)==ESP_OK;
        dsps_fft4r_deinit_fc32();
        if(!ok)return false;
        for(int k=0;k<n;++k)if(std::abs(raw[2*k]-1.f)>3e-6 || raw[2*k+1]!=0)return false;
        for(int i=0;i<4;++i)if(table[i]!=-321.f || table[4+4*n+i]!=-321.f || data[i]!=-321.f || data[4+2*n+i]!=-321.f)return false;
        if(!heap_caps_check_integrity_all(true))return false;
        vTaskDelay(1);
    }
    std::printf("fft_vendor_memory result=PASS cycles=20 table_floats=4*N data_floats=2*N guarded_external_tables=1\n");
    return true;
}
struct HeapChurn { std::atomic<bool> stop{false},done{false}; std::atomic<unsigned> cycles{0}; };
void heap_churn(void* opaque) {
    auto& state=*static_cast<HeapChurn*>(opaque);
    unsigned value=17;
    while(!state.stop.load()) {
        value=value*1664525u+1013904223u;
        const auto bytes=32u+(value&8191u);
        if(auto* allocation=heap_caps_malloc(bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)) {
            std::memset(allocation,0x5a,bytes);heap_caps_free(allocation);
        }
        ++state.cycles;
        vTaskDelay(1);
    }
    state.done=true;
    vTaskDelete(nullptr);
}
bool verify_plan_memory_body(unsigned& transforms) {
    using namespace dsp_kernels;
    PreparedRfft plan;
    std::uint32_t random=0x12345678u;
    for(std::size_t n:{512,1024,2048,4096}) {
        std::vector<float> input(n),expected(n+2),storage(n+10,-321.f);
        const std::span<float> out(storage.data()+4,n+2);
        FFT oracle;if(!oracle.prepare(n))return false;
        for(unsigned family=0;family<6;++family) {
            for(std::size_t i=0;i<n;++i) {
                random^=random<<13;random^=random>>17;random^=random<<5;
                switch(family) {
                case 0:input[i]=0;break;
                case 1:input[i]=i==n/3?.7f:0.f;break;
                case 2:input[i]=.17f;break;
                case 3:input[i]=i%2?-.11f:.11f;break;
                case 4:{double x=0;for(unsigned t=0;t<7;++t)x+=.2/(t+1)*std::cos(6.283185307179586*(3.25+13*t)*i/n+.17*t);input[i]=float(x);break;}
                default:input[i]=float(int(random&65535u)-32768)/65536.f;break;
                }
            }
            if(!oracle.process(input,expected))return false;
            for(bool radix4:{false,true}) for(bool platform:{false,true}) {
                const PreparedRfft::Options options{platform};
                if(!plan.prepare(n,radix4,PreparedRfft::Split::Automatic,&options))return false;
                for(unsigned repeat=0;repeat<8;++repeat) {
                    if(!plan.process(input,out))return false;
                    ++transforms;
                    for(std::size_t i=0;i<out.size();++i)
                        if(!std::isfinite(out[i]) || std::abs(double(out[i])-expected[i])>.0003+5e-6*std::abs(double(expected[i])))return false;
                    for(unsigned i=0;i<4;++i)if(storage[i]!=-321.f || storage[n+6+i]!=-321.f)return false;
                }
                plan.reset();
                if(!heap_caps_check_integrity_all(true))return false;
                vTaskDelay(1);
            }
        }
    }
    return true;
}
bool verify_instance_oom() {
    using namespace dsp_kernels;
    std::vector<float> input(4096),output(4098,-321.f);
    std::array<void*,128> reserved{};
    unsigned count=0;
    while(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>=4098*sizeof(float) && count<reserved.size()) {
        auto* block=heap_caps_malloc(8192,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(!block)break;
        reserved[count++]=block;
    }
    const bool forced=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<4098*sizeof(float);
    PreparedRfft plan;
    const bool rejected=!plan.prepare(4096,true) && plan.samples()==0 && !plan.process(input,output);
    for(unsigned i=0;i<count;++i)heap_caps_free(reserved[i]);
    plan.reset();
    const bool atomic=std::all_of(output.begin(),output.end(),[](float x){return x==-321.f;});
    const bool retry=plan.prepare(4096,true) && plan.process(input,output);
    const bool ok=forced && rejected && atomic && retry && heap_caps_check_integrity_all(true);
    std::printf("fft_instance_oom result=%s reservations=%u rejected=%d atomic=%d retry=%d\n",ok?"PASS":"FAIL",count,rejected,atomic,retry);
    return ok;
}
bool verify_memory() {
    if(!verify_vendor_memory())return false;
    // Warm persistent table ownership before measuring instance lifetime.
    {dsp_kernels::FFT r2,r4;if(!r2.prepare(4096)||!r4.prepare(4096,true))return false;}
    if(!verify_instance_oom())return false;
    vTaskDelay(2);
    const auto before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    HeapChurn state;
    if(xTaskCreatePinnedToCore(heap_churn,"fft_heap",2048,&state,2,nullptr,0)!=pdPASS)return false;
    unsigned transforms=0;
    const bool ok=verify_plan_memory_body(transforms);
    state.stop=true;
    while(!state.done.load())vTaskDelay(1);
    vTaskDelay(3); // Allow idle to reclaim the task stack/TCB.
    const auto after=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    std::printf("fft_memory result=%s transforms=%u families=6 heap_churn=%u free_before=%u free_after=%u stack_free=%u\n",
        ok && after>=before && heap_caps_check_integrity_all(true)?"PASS":"FAIL",transforms,state.cycles.load(),unsigned(before),unsigned(after),unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    return ok && after>=before && heap_caps_check_integrity_all(true);
}
}
