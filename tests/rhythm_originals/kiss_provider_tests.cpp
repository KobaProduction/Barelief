#include <kiss_backend.h>
#include "kiss_generic_source_golden.h"
#include <dsp_kernels/tempogram.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

namespace {
void tracking_self_check();
thread_local bool watching=false;
thread_local std::size_t allocations=0;
void require(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}
}
void allocation() noexcept {if(watching)++allocations;}
void* allocate(std::size_t bytes) {
    allocation();if(void* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();
}
const dsp_kernels::FourierBackendFactory& factory() {return desktop_fourier::kiss_backend_factory();}
using Transform=dsp_kernels::FourierBackendTransform;
using Owner=dsp_kernels::PreparedFourierBackend;
void close(std::span<const float> a,std::span<const float> b) {
    require(a.size()==b.size(),"equal value shape");
    for(std::size_t i=0;i<a.size();++i)require(std::abs(a[i]-b[i])<=2e-5f*(1+std::abs(b[i])),"Fourier inverse normalization");
}
void tiny_and_atomic() {
    Owner operation;std::array<float,2> complex{1,-2},value{};
    require(operation.prepare({1,Transform::ComplexForward},factory()),"complex N1 prepare");
    require(operation.process(complex,value) && complex==value,"complex N1 source identity");
    require(operation.prepare({1,Transform::ComplexInverse},factory()) && operation.process(complex,value) && value==complex,"complex N1 inverse");
    std::array<float,2> real{1,2};std::array<float,4> bins{};
    require(operation.prepare({2,Transform::RealForward},factory()) && operation.process(real,bins),"real N2 prepare/process");
    require(bins==std::array<float,4>{3,0,-1,0},"real N2 transform values");
    require(operation.prepare({2,Transform::RealInverse},factory()) && operation.process(bins,value) && value==real,"real N2 inverse 1/N");
    bins[1]=1;value.fill(71);
    require(!operation.process(bins,value) && value==std::array<float,2>{71,71},"DC imaginary rejects atomically");
    bins[1]=0;bins[3]=1;require(!operation.process(bins,value) && value[0]==71,"Nyquist imaginary rejects atomically");
    bins[3]=0;bins[0]=std::numeric_limits<float>::infinity();require(!operation.process(bins,value) && value[0]==71,"nonfinite atomic output");
    require(!operation.prepare({3,Transform::RealForward},factory()) && !operation.ready(),"invalid prepare clears ready");
    dsp_kernels::FourierBackendResources resources{7,7,7,7};
    require(!Owner::query({3,Transform::RealForward},factory(),resources) && resources.owned_bytes==7,"odd real query rejects without mutation");
    require(!Owner::query({2,Transform(99)},factory(),resources),"invalid transform enum");
}
void no_heap(Owner& forward,Owner& inverse,std::span<const float> input,std::span<float> spectrum,std::span<float> output) {
    allocations=0;watching=true;
    const bool ok=forward.process(input,spectrum) && inverse.process(spectrum,output);
    watching=false;require(ok && !allocations,"prepared forward/inverse no C++ new or vendor malloc");
}
void generic_radix_plans() {
    for(const auto n:std::array<std::size_t,8>{3,6,7,11,49,98,1103,2206}){
        if(n>dsp_kernels::max_fft_samples)continue;
        std::vector<float> input(2*n),spectrum(2*n),output(2*n);input[0]=.25f;input[1]=-.5f;input.back()=.125f;
        Owner forward,inverse;dsp_kernels::FourierBackendResources bound;
        require(Owner::query({n,Transform::ComplexForward},factory(),bound) &&
            forward.prepare({n,Transform::ComplexForward},factory()) && forward.workspace_bytes()<=bound.owned_bytes,"generic complex cold scratch bound");
        require(inverse.prepare({n,Transform::ComplexInverse},factory()),"generic complex inverse");
        no_heap(forward,inverse,input,spectrum,output);close(output,input);
        auto copy=forward;require(copy.process(input,spectrum),"generic independent scratch clone");
        if(n%2)continue;
        std::vector<float> real(n),positive(n+2),restored(n);real[0]=.75f;real[n-1]=-.25f;
        require(forward.prepare({n,Transform::RealForward},factory()) && inverse.prepare({n,Transform::RealInverse},factory()),"source even generic real plan");
        no_heap(forward,inverse,real,positive,restored);close(restored,real);
    }
}
void genuine_source_2206() {
    if(dsp_kernels::max_fft_samples<2206)return;
    std::vector<float> input(2206),output(2208);input[0]=.75f;input.back()=-.25f;
    Owner operation;require(operation.prepare({2206,Transform::RealForward},factory()) && operation.process(input,output),"actual source-valid N2206 FFT");
    for(std::size_t i=0;i<kiss_generic_source::bins.size();++i){
        const auto bin=kiss_generic_source::bins[i];
        require(std::bit_cast<std::uint32_t>(output[2*bin])==kiss_generic_source::values[i][0] &&
            std::bit_cast<std::uint32_t>(output[2*bin+1])==kiss_generic_source::values[i][1],"genuine original prime-subplan FFT bits");
    }
}
void plans_pow2() {
    const auto maximum=std::min(std::size_t{16384},dsp_kernels::max_fft_samples);
    dsp_kernels::FourierBackendResources rejected;
    require(!Owner::query({maximum*2,Transform::ComplexForward},factory(),rejected),"configured provider capacity rejects without fallback");
    for(std::size_t n=1;n<=maximum;n*=2){
        std::vector<float> complex(2*n),bins(2*n),back(2*n);complex[0]=.25f;complex[1]=-.5f;
        Owner forward,inverse;dsp_kernels::FourierBackendResources bound;
        require(Owner::query({n,Transform::ComplexForward},factory(),bound) && forward.prepare({n,Transform::ComplexForward},factory()),"pow2 complex query/prepare");
        require(forward.workspace_bytes()<=bound.owned_bytes && bound.cold_peak_bytes>=bound.owned_bytes && !bound.shared_bytes,"complex owned resource bound");
        require(inverse.prepare({n,Transform::ComplexInverse},factory()),"pow2 complex inverse prepare");
        no_heap(forward,inverse,complex,bins,back);close(back,complex);
        if(n<2)continue;
        std::vector<float> real(n),positive(n+2),restored(n);real[0]=.75f;real[n-1]=-.25f;
        require(forward.prepare({n,Transform::RealForward},factory()) && inverse.prepare({n,Transform::RealInverse},factory()),"pow2 real pair");
        no_heap(forward,inverse,real,positive,restored);close(restored,real);
        auto cloned=forward;std::fill(positive.begin(),positive.end(),0);require(cloned.process(real,positive),"independent cold clone process");
        require(inverse.process(positive,restored),"independent inverse after clone");close(restored,real);
    }
}
void clone_factory_lifetime() {
    struct TemporaryFactory final:dsp_kernels::FourierBackendFactory {
        const char* id()const noexcept override{return factory().id();}
        bool query(dsp_kernels::FourierBackendRequest r,dsp_kernels::FourierBackendResources& b)const noexcept override{return factory().query(r,b);}
        std::unique_ptr<dsp_kernels::FourierBackend> prepare(dsp_kernels::FourierBackendRequest r)const override{return factory().prepare(r);}
    };
    Owner operation;
    {TemporaryFactory temporary;require(operation.prepare({2,Transform::RealForward},temporary),"cold borrowed factory");}
    auto cloned=operation;operation=Owner{};
    const std::array<float,2> input{1,2};std::array<float,4> output{};
    require(cloned.process(input,output) && output[0]==3,"clone owns plan after factory/parent destruction");
    require(!cloned.process(std::span(output).first(2),output),"overlapping caller spans rejected");
}
void default_and_compositions() {
    namespace d=dsp_kernels;d::TemporalWindowConfig c;c.length=64;c.hop=32;c.center=false;
    std::vector<float> input(256),ones(64,1),a(66),b(66);input[2]=1;input[89]=.5f;
    d::PreparedFourierTempogram baseline,explicit_default,external;
    require(baseline.prepare(c,ones) && explicit_default.prepare(c,ones,0,false,nullptr),"default plan pair");
    require(baseline.process_frame(input,0,a) && explicit_default.process_frame(input,0,b) && a==b,"nullptr preserves default arithmetic");
    require(external.prepare(c,ones,0,false,&factory()) && external.process_frame(input,0,b),"source FFT composition");
    c.precision=d::NumericalPrecision::DesktopReference;
    require(!external.prepare(c,ones,0,false,&factory()) && external.view().complex_values.empty(),"Double plus external rejects with empty view");
    d::PreparedVariableAutocorrelation correlation;std::size_t bound=0;std::array<float,1> one{2},lag{};
    require(decltype(correlation)::storage_bytes(143,d::CorrelationNormalization::Unbiased,bound,&factory()) &&
        correlation.prepare(143,d::CorrelationNormalization::Unbiased,&factory()) && correlation.workspace_bytes()<=bound,"variable ACF provider bound");
    require(correlation.process(one,lag) && lag[0]==4 && correlation.active_fft_size()==2,"variable ACF tiny admitted N2");
    auto copied=correlation;require(copied.process(one,lag) && lag[0]==4,"variable ACF independent clone");

}
}
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t n) {allocation();return __real_malloc(n);}
void* operator new(std::size_t n) {return allocate(n);}
void* operator new[](std::size_t n) {return allocate(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
void tracking_self_check() {
    void* (*volatile cpp_allocate)(std::size_t)=static_cast<void* (*)(std::size_t)>(&::operator new);
    void* (*volatile c_allocate)(std::size_t)=&std::malloc;
    allocations=0;watching=true;
    void* a=cpp_allocate(8);void* b=c_allocate(8);
    watching=false;require(a && b && allocations>=3,"C++ and vendor C allocation hooks active");
    ::operator delete(a);std::free(b);
}
}
int main() {
    tracking_self_check();tiny_and_atomic();plans_pow2();generic_radix_plans();genuine_source_2206();clone_factory_lifetime();default_and_compositions();
    std::puts("KISS prepared provider: tiny/atomic/pow2/nohotheap/clone/default/composition PASS");
}
