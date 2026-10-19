#include <dsp_kernels/dsp.h>
#include "../libs/dsp_kernels/src/backend.h"
#include <array>
#include <cstdio>
#include <cstdlib>
namespace {
std::array<int,4> calls{};
int fail = -1;
void require(bool value) { if (!value) std::abort(); }
int record(int id,float* data,std::size_t n) {require(data && n==4096);++calls[id];return fail==id?-1:0;}
}
int fft2_dispatch(float* p,std::size_t n){return record(0,p,n);}
int reverse2_dispatch(float* p,std::size_t n){return record(1,p,n);}
int fft4_dispatch(float* p,std::size_t n){return record(2,p,n);}
int reverse4_dispatch(float* p,std::size_t n){return record(3,p,n);}
int main() {
    alignas(16) float storage[8192]{};
    require(dsp_kernels::compiled_backend()==dsp_kernels::Backend::EspDsp);
    require(dsp_kernels::detail::transform(storage,4096,false) && calls==std::array{1,1,0,0});
    require(dsp_kernels::detail::transform(storage,4096,true) && calls==std::array{1,1,1,1});
    for (int id=0;id<4;++id) {
        calls={};fail=id;
        require(!dsp_kernels::detail::transform(storage,4096,id>=2));
        require(calls[id]==1);
        if(id%2==0)require(calls[id+1]==0);
    }
    std::puts("ESP-DSP radix2/radix4 SDK dispatch and failure propagation PASS (host stub, not hardware)");
}
