#include <rhythm_reference_ports/pcm_predictive_beat_tracker.h>
#include <kiss_backend.h>
#include <sinc_backend.h>
#include "btrack_comb_source_golden.h"
#include "sinc_source_arithmetic_golden.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
namespace {
namespace r=rhythm_reference_ports;
thread_local bool watching=false;
thread_local std::size_t allocations=0;
void check(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
void allocation() noexcept {if(watching)++allocations;}
void* allocate(std::size_t count){allocation();if(auto p=std::malloc(count?count:1))return p;throw std::bad_alloc();}
const dsp_kernels::FourierBackendFactory* factory(){return &desktop_fourier::kiss_backend_factory();}
r::PcmPredictiveOnsetConfig reference_config(){
    r::PcmPredictiveOnsetConfig c{4,8};c.precision=dsp_kernels::NumericalPrecision::DesktopReference;return c;
}
void admission() {
    auto c=reference_config();std::size_t bytes=71;
    check(!r::PreparedPcmPredictiveOnset::storage_bytes(c,bytes) && bytes==71,"reference requires explicit factory");
    c.method=r::PredictiveOnsetMethod(99);
    check(!r::PreparedPcmPredictiveOnset::storage_bytes(c,bytes,factory()) && bytes==71,"invalid reference method rejects");
    c=reference_config();c.window=r::PredictiveWindowShape(99);
    check(!r::PreparedPcmPredictiveOnset::storage_bytes(c,bytes,factory()),"invalid reference window rejects");
    c=reference_config();c.precision=dsp_kernels::NumericalPrecision(99);
    check(!r::PreparedPcmPredictiveOnset::storage_bytes(c,bytes,factory()),"invalid precision rejects");
    c.precision=dsp_kernels::NumericalPrecision::PortableFloat32;
    check(!r::PreparedPcmPredictiveOnset::storage_bytes(c,bytes,factory()),"portable has no implicit factory switch");
}
void prepared_clone_reset() {
    r::PreparedPcmPredictiveOnset operation;const auto c=reference_config();std::size_t bytes=0;
    check(decltype(operation)::storage_bytes(c,bytes,factory()) && operation.prepare(c,factory()) && operation.workspace_bytes()<=bytes,"source frontend resource bound");
    const std::array<float,4> pcm{.25f,-.5f,.125f,.375f};float first=0,value=0,other=0;
    check(operation.process_hop(pcm,first),"source first hop");auto copy=operation;
    check(operation.process_hop(pcm,value) && copy.process_hop(pcm,other) && value==other &&
        operation.reference_stage().value()==copy.reference_stage().value(),"independent backend/state clone");
    copy.restart();check(copy.process_hop(pcm,value) && value==first,"restart retains source profile, restores fresh state");
    check(!copy.set_method(r::PredictiveOnsetMethod(99)) && copy.config().method==c.method,"warm invalid method preserves source state");
    auto before=copy;auto invalid=pcm;invalid[0]=std::numeric_limits<float>::quiet_NaN();
    check(!copy.process_hop(invalid,value) && copy.process_hop(pcm,value) && before.process_hop(pcm,other) && value==other,"nonfinite PCM rejects before history advance");
}
void modes_and_windows() {
    const std::array<float,4> input{1,0,0,0};
    for(unsigned shape=0;shape<5;++shape)for(unsigned method=0;method<10;++method){
        auto c=reference_config();c.window=r::PredictiveWindowShape(shape);c.method=r::PredictiveOnsetMethod(method);
        r::PreparedPcmPredictiveOnset operation;std::size_t bound=0;float value=0;
        check(decltype(operation)::storage_bytes(c,bound,factory()) && operation.prepare(c,factory()) &&
            operation.workspace_bytes()<=bound,"all10 source methods/5windows admitted and bounded");
        allocations=0;watching=true;const bool ok=operation.process_hop(input,value);watching=false;
        check(ok && !allocations && std::isfinite(value),"all source modes first hop finite with no hot heap");
    }
}
void warm_energy_history() {
    auto c=reference_config();c.method=r::PredictiveOnsetMethod::EnergyEnvelope;r::PreparedPcmPredictiveOnset operation;
    const std::array<float,4> input{1,0,0,0};float value=0;check(operation.prepare(c,factory()),"warm source history prepare");
    check(operation.process_hop(input,value) && value==1,"raw unwindowed source energy");
    check(operation.set_method(r::PredictiveOnsetMethod::EnergyDifference) && operation.process_hop(input,value) && value==2,
        "envelope does not advance separate energy-difference prior");
    check(operation.set_method(r::PredictiveOnsetMethod::EnergyEnvelope) && operation.process_hop(input,value) && value==2,
        "warm energy setter retains PCM history");
    check(operation.set_method(r::PredictiveOnsetMethod::EnergyDifference) && operation.process_hop(input,value) && value==0,
        "separate energy-difference history survives another mode");
}
void hot_replays() {
    r::PreparedPcmPredictiveOnset operation;check(operation.prepare(reference_config(),factory()),"prepare hot source frontend");
    const std::array<float,4> pcm{.25f,-.5f,.125f,.375f};float result=0;bool ok=true;
    allocations=0;watching=true;
    for(unsigned i=0;i<64;++i)ok=operation.process_hop(pcm,result) && ok;
    watching=false;check(ok && !allocations,"source frontend no hot C++ new or KISS malloc");
}
void tracker_geometry() {
    r::PreparedPcmPredictiveBeatTracker tracker;auto c=reference_config();c.hop_samples=512;c.frame_samples=1024;
    std::size_t bytes=0;const auto& resampler=desktop_resampling::sinc_backend_factory();
    check(decltype(tracker)::storage_bytes(resampler,c,bytes,factory()) && tracker.prepare(resampler,c,factory()) && tracker.workspace_bytes()<=bytes,"whole source frontend forwarding/query");
    check(!tracker.update_hop_and_frame(1024,2048) && tracker.frontend().config().frame_samples==1024,"geometry without source factory rejects atomically");
    check(tracker.update_hop_and_frame(1024,2048,factory()) && tracker.frontend().config().precision==c.precision,"explicit geometry preserves source profile");
    c.hop_samples=1024;c.frame_samples=2048;
    check(decltype(tracker)::storage_bytes(resampler,c,bytes,factory()) && tracker.workspace_bytes()<=bytes,"changed geometry resource bound");
    auto clone=tracker.clone();check(bool(clone) && clone->frontend().config().precision==c.precision && clone->restart(),"whole clone/restart keeps profile");
}
void source_input_rounding() {
    struct Callback {
        static bool process(void*,std::span<const float>,std::span<float> output) noexcept {std::fill(output.begin(),output.end(),0);return true;}
    };
    r::PreparedPredictiveBeatTracker tracker;check(tracker.prepare(512,{nullptr,&Callback::process}),"rounding tracker prepare");
    r::BeatPredictionObservation observation;const double value=566.2577179394652;
    check(tracker.process_reference(value,observation) && tracker.onset_window().back()==566.2578125f,"actual source ODF abs/epsilon rounds once");
    const auto before=tracker.onset_window().back();
    check(!tracker.process_reference(std::numeric_limits<double>::max(),observation) && tracker.onset_window().back()==before,"reference range guard before history advance");
}
void source_comb() {
    r::PreparedCombTempoTransition operation,portable;std::array<float,512> input{};
    for(std::size_t i=0;i<input.size();++i)input[i]=std::bit_cast<float>(btrack_comb_source::input[i]);
    std::size_t bound=0;float period=0;
    check(decltype(operation)::storage_bytes(2048,bound,dsp_kernels::NumericalPrecision::DesktopReference,factory()) &&
        operation.prepare(2048,dsp_kernels::NumericalPrecision::DesktopReference,factory()) && operation.workspace_bytes()<=bound,"mixed comb query/owned bounds");
    check(operation.process(input,period),"mixed source threshold/KISS/ACF/comb process");
    const auto& source=operation.reference_stage();
    for(std::size_t i=0;i<btrack_comb_source::lags.size();++i)
        check(std::bit_cast<std::uint64_t>(source.autocorrelation()[btrack_comb_source::lags[i]])==btrack_comb_source::acf[i],"actual genuine mixed ACF bits");
    for(std::size_t i=0;i<btrack_comb_source::bins.size();++i)
        check(std::bit_cast<std::uint64_t>(source.response()[btrack_comb_source::bins[i]])==btrack_comb_source::comb[i],"actual genuine Double comb bits");
    check(portable.prepare(2048) && portable.process(input,period),"legacy PortableFloat32 comb process");
    for(std::size_t i=0;i<btrack_comb_source::bins.size();++i)
        check(std::bit_cast<std::uint32_t>(portable.response()[btrack_comb_source::bins[i]])==btrack_comb_source::portable_comb[i],"prior Float32 comb arithmetic stays bitwise");
}
void source_comb_replay() {
    r::PreparedCombTempoTransition operation;std::array<float,512> input{};float period=0;
    for(std::size_t i=0;i<input.size();++i)input[i]=std::bit_cast<float>(btrack_comb_source::input[i]);
    check(operation.prepare(2048,dsp_kernels::NumericalPrecision::DesktopReference,factory()) && operation.process(input,period),"source comb clone seed");
    auto copy=operation;float other=0;
    check(operation.process(input,period) && copy.process(input,other) && period==other &&
        std::equal(operation.reference_stage().probabilities().begin(),operation.reference_stage().probabilities().end(),copy.reference_stage().probabilities().begin()),"owned comb plans/state clone independent");
    operation.reset();copy.reset();check(operation.fix_tempo(120) && copy.fix_tempo(120),"source fixed tempo reset");
    allocations=0;watching=true;bool ok=true;
    for(unsigned i=0;i<64;++i)ok=operation.process(input,period) && ok;
    watching=false;check(ok && !allocations,"mixed comb64replays have no hot new/malloc");
    check(operation.set_hop_samples(512) && operation.set_tempo(100),"source hop/prior controls retain prepared plans");operation.release_tempo();
    std::size_t bound=71;
    check(!decltype(operation)::storage_bytes(512,bound,dsp_kernels::NumericalPrecision::DesktopReference) && bound==71,"reference comb requires explicit factory");
}
void source_sinc() {
    std::array<float,512> input{},actual{},portable{};
    for(std::size_t i=0;i<input.size();++i)input[i]=std::bit_cast<float>(sinc_source_arithmetic::input[i]);
    dsp_kernels::ResamplingWindowConfig config{512,512,1};dsp_kernels::PreparedResampler legacy,reference;
    const auto* backend=&desktop_resampling::sinc_backend_factory();std::size_t count=0;
    check(legacy.prepare_window(config,backend) && legacy.process_window(input,512,portable,512,count) && count==512,"default sinc window");
    config.precision=dsp_kernels::NumericalPrecision::DesktopReference;dsp_kernels::ResamplerResources bound;
    check(decltype(reference)::window_resource_requirements(config,backend,bound) && reference.prepare_window(config,backend) &&
        reference.resources().owned_bytes<=bound.owned_bytes,"source sinc precision/query/inline sizeof accounted");
    check(reference.process_window(input,512,actual,512,count) && count==512,"source Double interpolation/sums toF32");
    for(std::size_t i=0;i<512;++i){
        check(std::bit_cast<std::uint32_t>(actual[i])==sinc_source_arithmetic::source[i],"actual genuine source sinc512bits");
        check(std::bit_cast<std::uint32_t>(portable[i])==sinc_source_arithmetic::portable[i],"prior Portable sinc512bits unchanged");
    }
    auto copy=reference;reference.reset();
    check(copy.window_config()->precision==config.precision && copy.process_window(input,512,portable,512,count) && actual==portable,"source sinc copy/reset retains precision/plan");
    allocations=0;watching=true;bool ok=true;
    for(unsigned i=0;i<8;++i)ok=reference.process_window(input,512,actual,512,count) && ok;
    watching=false;check(ok && !allocations,"source sinc hotreplays no C++ or C heap");
}
void sinc_precision_guards() {
    dsp_kernels::ResamplingWindowConfig config{512,512,1};config.precision=dsp_kernels::NumericalPrecision(99);
    dsp_kernels::PreparedResampler operation;const auto* factory=&desktop_resampling::sinc_backend_factory();
    check(!operation.prepare_window(config,factory),"unknown window precision rejects cold");
    config={2,3,1,dsp_kernels::ResamplingWindowMode::EndpointLinear,2,dsp_kernels::NumericalPrecision::DesktopReference};
    check(!operation.prepare_window(config),"EndpointLinear rejects unsupported reference policy");
    config.precision=dsp_kernels::NumericalPrecision::PortableFloat32;
    check(operation.prepare_window(config),"EndpointLinear default retains original admission");
}
void tracking_self_check();
}
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t n){allocation();return __real_malloc(n);}
void* operator new(std::size_t n){return allocate(n);}
void* operator new[](std::size_t n){return allocate(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
void tracking_self_check() {
    void* (*volatile cpp_allocate)(std::size_t)=static_cast<void* (*)(std::size_t)>(&::operator new);
    void* (*volatile c_allocate)(std::size_t)=&std::malloc;allocations=0;watching=true;
    void* cpp=cpp_allocate(8);void* c=c_allocate(8);watching=false;
    check(cpp && c && allocations>=3,"C++ and vendor malloc tracking active");::operator delete(cpp);std::free(c);
}
}
int main(){tracking_self_check();admission();prepared_clone_reset();modes_and_windows();warm_energy_history();hot_replays();tracker_geometry();source_input_rounding();source_comb();source_comb_replay();source_sinc();sinc_precision_guards();
    std::puts("BTrack explicit source frontend admission/copy/reset/geometry/resources/nohotheap PASS");}
