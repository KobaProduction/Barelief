#include "../libs/dsp_kernels/src/backend.h"
#include <dsp_kernels/dsp.h>
#include "dsps_fft4r.h"
#include "dsps_fft2r.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
struct Allocation { unsigned char* pointer=nullptr; std::size_t bytes=0; };
std::array<Allocation,8> live{};
unsigned call_count=0, fail_call=0;
bool guard_corruption=false;
void require(bool condition,const char* message) {
    if(!condition){std::fprintf(stderr,"%s\n",message);std::exit(1);}
}
bool intact(const Allocation& a) {
    for(std::size_t i=0;i<64;++i) if(a.pointer[a.bytes+i]!=0xa5)return false;
    return true;
}
void empty() { for(const auto& a:live) require(!a.pointer,"Initializer leaked an owned allocation"); }
}
extern "C" void* fft_fault_malloc(std::size_t bytes) {
    if(++call_count==fail_call)return nullptr;
    // Quarantine space makes the known small->large retry overflow observable
    // without corrupting the actual host heap during vendor reproduction.
    auto* pointer=static_cast<unsigned char*>(std::malloc(bytes+16384));
    if(!pointer)return nullptr;
    std::memset(pointer+bytes,0xa5,64);
    for(auto& a:live)if(!a.pointer){a={pointer,bytes};return pointer;}
    std::abort();
}
extern "C" void fft_fault_free(void* pointer) {
    if(!pointer)return;
    for(auto& a:live)if(a.pointer==pointer){guard_corruption|=!intact(a);std::free(pointer);a={};return;}
    std::abort();
}
int main(int argc,char**) {
    if(argc>1) {
        fail_call=2;
        require(dsps_fft4r_init_fc32(nullptr,16)!=ESP_OK,"Expected second-allocation failure");
        const auto* small=dsps_fft4r_w_table_fc32;
        call_count=0;fail_call=0;
        require(dsps_fft4r_init_fc32(nullptr,64)==ESP_OK,"Vendor retry failed");
        require(small==dsps_fft4r_w_table_fc32 && call_count==1,"Expected unsafe twiddle reuse");
        dsps_fft4r_deinit_fc32();
        require(guard_corruption,"Expected overwrite of the original 16-point table guard");
        empty();
        std::puts("CONFIRMED: FFT4 second malloc failure -> retry larger size reuses undersized twiddles and overwrites guard");
        return 0;
    }
    // Profiles above the vendor lookup-table limit use direct permutation;
    // there is no second (bit-reversal table) allocation in those profiles.
    for(bool radix4:{false,true}) for(unsigned failed=1;failed<=((radix4?dsp_kernels::max_radix4_samples:dsp_kernels::max_fft_samples)<=4096?2u:1u);++failed) {
        call_count=0;fail_call=failed;
        require(!dsp_kernels::detail::prepare_backend(16,radix4),"Allocation failure accepted");
        require(!(radix4?dsps_fft4r_initialized:dsps_fft2r_initialized),"Failed init published a ready table");
        empty();
        require((radix4?dsps_fft4r_w_table_fc32:dsps_fft_w_table_fc32)==nullptr && (radix4?dsps_fft4r_w_table_size:dsps_fft_w_table_size)==0,"Failed init left stale pointers/capacity");
        call_count=0;fail_call=0;
        require(dsp_kernels::detail::prepare_backend(64,radix4),"Clean retry failed");
        if(radix4)dsps_fft4r_deinit_fc32();else dsps_fft2r_deinit_fc32();
        empty();
        require(!guard_corruption,"Recovered initialization overwrote its allocation");
    }
    std::puts("FFT2/4 transactional init: every allocation in the configured profile, cleanup, retry and guards PASS");
}
