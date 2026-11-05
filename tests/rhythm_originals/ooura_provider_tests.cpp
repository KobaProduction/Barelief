#include <ooura_backend.h>
#include "ooura_source_fft_golden.h"
#include <dsp_kernels/dsp.h>
#include <rhythm_reference_ports/pcm_onset_sequence.h>
#include <rhythm_reference_ports/pcm_context_tempo.h>
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
using Owner=dsp_kernels::PreparedFourierBackend;
using Transform=dsp_kernels::FourierBackendTransform;
thread_local bool watching=false;
thread_local std::size_t allocations=0;
void require(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"Ooura provider: %s\n",message);std::exit(1);}
}
void allocation() noexcept {if(watching)++allocations;}
void* allocate(std::size_t bytes) {
    allocation();if(void* value=std::malloc(bytes?bytes:1))return value;throw std::bad_alloc();
}
const dsp_kernels::FourierBackendFactory& factory() {return desktop_fourier::ooura_backend_factory();}
void close(std::span<const float> actual,std::span<const float> expected) {
    require(actual.size()==expected.size(),"shape");
    for(std::size_t i=0;i<actual.size();++i)require(std::abs(actual[i]-expected[i])<=2e-5f*(1+std::abs(expected[i])),"inverse normalization");
}
void no_heap(Owner& forward,Owner& inverse,std::span<const float> input,std::span<float> bins,std::span<float> restored) {
    allocations=0;watching=true;
    const bool ok=forward.process(input,bins) && inverse.process(bins,restored);
    watching=false;require(ok && allocations==0,"cold materialized forward/inverse has no hot allocation");
}
void admission_and_atomic() {
    dsp_kernels::FourierBackendResources bound{7,7,7,7};
    for(auto request:std::array<dsp_kernels::FourierBackendRequest,6>{{{1,Transform::RealForward},{3,Transform::RealForward},
        {6,Transform::RealInverse},{2,Transform::ComplexForward},{2,Transform::ComplexInverse},{2,Transform(99)}}})
        require(!Owner::query(request,factory(),bound) && bound.owned_bytes==7,"unsupported query rejects without publication");
    Owner operation;std::array<float,2> real{1,2},restored{71,71};std::array<float,4> bins{};
    require(operation.prepare({2,Transform::RealForward},factory()) && operation.process(real,bins),"N2 real forward");
    require(bins==std::array<float,4>{3,0,-1,0},"N2 packed endpoints");
    require(operation.prepare({2,Transform::RealInverse},factory()) && operation.process(bins,restored) && restored==real,"N2 source inverse");
    restored.fill(71);bins[1]=1;
    require(!operation.process(bins,restored) && restored==std::array<float,2>{71,71},"non-Hermitian DC atomic");
    bins[1]=0;bins[3]=1;require(!operation.process(bins,restored) && restored[0]==71,"non-Hermitian Nyquist atomic");
    bins[3]=0;bins[0]=std::numeric_limits<float>::infinity();require(!operation.process(bins,restored) && restored[0]==71,"nonfinite atomic");
    bins[0]=1;require(!operation.process(std::span(bins).first(3),restored) && restored[0]==71,"size atomic");
    require(!operation.process(bins,std::span(bins).first(2)),"overlap atomic");
    require(!operation.prepare({6,Transform::RealForward},factory()) && !operation.ready(),"failed prepare clears holder");
}
void plans_and_clones() {
    const auto limit=std::min(dsp_kernels::max_fft_samples,std::size_t{16384});
    dsp_kernels::FourierBackendResources bound;
    require(!Owner::query({limit*2,Transform::RealForward},factory(),bound),"declared capacity rejects without fallback");
    for(std::size_t size=2;size<=limit;size*=2){
        std::vector<float> input(size),bins(size+2),restored(size),copied(size+2);input.front()=.75f;input.back()=-.25f;
        Owner forward,inverse;
        require(Owner::query({size,Transform::RealForward},factory(),bound) && forward.prepare({size,Transform::RealForward},factory()),"real query/prepare");
        require(forward.workspace_bytes()==bound.owned_bytes && bound.cold_peak_bytes==bound.owned_bytes && bound.shared_bytes==0,"owned object/ip/table/data accounted");
        require(inverse.prepare({size,Transform::RealInverse},factory()),"inverse prepare");
        no_heap(forward,inverse,input,bins,restored);close(restored,input);
        auto clone=forward;forward=Owner{};
        allocations=0;watching=true;const bool ok=clone.process(input,copied);watching=false;
        require(ok && !allocations && copied==bins,"independent clone after parent destruction, no lazy heap");
    }
}
void clone_factory_lifetime() {
    struct Temporary final:dsp_kernels::FourierBackendFactory {
        const char* id()const noexcept override{return factory().id();}
        bool query(dsp_kernels::FourierBackendRequest r,dsp_kernels::FourierBackendResources& b)const noexcept override{return factory().query(r,b);}
        std::unique_ptr<dsp_kernels::FourierBackend> prepare(dsp_kernels::FourierBackendRequest r)const override{return factory().prepare(r);}
    };
    Owner operation;{Temporary temporary;require(operation.prepare({2,Transform::RealForward},temporary),"cold borrowed factory");}
    auto copy=operation;operation=Owner{};std::array<float,4> output{};
    require(copy.process(std::array<float,2>{1,2},output) && output[0]==3,"clone independent of borrowed factory lifetime");
}
void genuine_source_bits() {
    if(dsp_kernels::max_fft_samples<64)return;
    std::array<float,64> input{};std::array<float,66> output{};
    for(std::size_t i=0;i<input.size();++i)input[i]=std::bit_cast<float>(ooura_source_fft::input[i]);
    Owner operation;require(operation.prepare({64,Transform::RealForward},factory()) && operation.process(input,output),"genuine source FFT row");
    for(std::size_t i=0;i<output.size();++i)require(std::bit_cast<std::uint32_t>(output[i])==ooura_source_fft::expected[i],"actual genuine Ooura FFT bits");
}
void hop_default_and_source() {
    namespace r=rhythm_reference_ports;r::PreparedHopSpectrum baseline,explicit_default,source;
    const r::HopSpectrumConfig config{64,32};std::size_t bytes=0;
    std::array<float,32> input{};input[0]=1;
    std::array<float,33> a{},b{},p{},q{};
    require(baseline.prepare(config) && explicit_default.prepare(config,{},nullptr),"default hop preparations");
    require(baseline.process_hop(input,a,p) && explicit_default.process_hop(input,b,q) && a==b && p==q,"nullptr default arithmetic unchanged");
    require(decltype(source)::storage_bytes(config,{},bytes,&factory()) && source.prepare(config,{},&factory()) && source.workspace_bytes()<=bytes,"source hop resource forwarding");
    allocations=0;watching=true;const bool ok=source.process_hop(input,b,q);watching=false;
    require(ok && !allocations,"prepared source HopSpectrum no hot allocation");
    auto copy=source;source.restart();copy.restart();
    require(source.process_hop(input,a,p) && copy.process_hop(input,b,q) && a==b && p==q,"source hop restart/clone equivalence");
}
void onset_and_tempo_forwarding() {
    namespace r=rhythm_reference_ports;r::PreparedPcmOnsetSequence onset;r::PreparedPcmContextTempo tempo;
    r::PcmOnsetSequenceConfig oc;oc.spectrum={64,32};r::PcmContextTempoConfig tc;tc.spectrum=oc.spectrum;
    std::size_t bytes=0;std::array<float,32> input{};input[0]=.75f;r::PcmOnsetObservation o,cloned;r::PcmContextTempoObservation t;
    require(decltype(onset)::storage_bytes(oc,bytes,&factory()) && onset.prepare(oc,&factory()) && onset.workspace_bytes()<=bytes,"onset cold provider forwarding/bound");
    require(decltype(tempo)::storage_bytes(tc,bytes,&factory()) && tempo.prepare(tc,&factory()) && tempo.workspace_bytes()<=bytes,"tempo cold provider forwarding/bound");
    allocations=0;watching=true;const bool ok=onset.process_hop(input,o) && tempo.process_hop(input,t);watching=false;
    require(ok && !allocations,"nested source onset/tempo no hot allocation");
    auto copy=onset;input[0]=-.25f;
    require(onset.process_hop(input,o) && copy.process_hop(input,cloned) && o.descriptor==cloned.descriptor &&
        o.peak.thresholded==cloned.peak.thresholded && o.onset.peak_position==cloned.onset.peak_position &&
        onset.last_onset_sample()==copy.last_onset_sample(),"nested clone preserves independent prepared history");
}
void tracking_self_check();
}
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t bytes) {allocation();return __real_malloc(bytes);}
void* operator new(std::size_t bytes) {return allocate(bytes);}
void* operator new[](std::size_t bytes) {return allocate(bytes);}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete[](void* value) noexcept {std::free(value);}
void operator delete(void* value,std::size_t) noexcept {std::free(value);}
void operator delete[](void* value,std::size_t) noexcept {std::free(value);}
namespace {
void tracking_self_check() {
    void* (*volatile cpp_allocate)(std::size_t)=static_cast<void* (*)(std::size_t)>(&::operator new);
    void* (*volatile c_allocate)(std::size_t)=&std::malloc;
    allocations=0;watching=true;void* a=cpp_allocate(8);void* b=c_allocate(8);watching=false;
    require(a && b && allocations>=3,"opaque C/new allocation hooks active");::operator delete(a);std::free(b);
}
}
int main() {
    tracking_self_check();admission_and_atomic();plans_and_clones();clone_factory_lifetime();genuine_source_bits();hop_default_and_source();onset_and_tempo_forwarding();
    std::puts("Ooura prepared source provider: real/atomic/cold/nohotheap/clone/default/forwarding PASS");
}
