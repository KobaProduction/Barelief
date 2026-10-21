#include <dsp_kernels/prepared_resampler.h>
#include "speex_backend.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include "test_assertions.h"
#define ASSERT(x) test_assertions::require(bool(x),#x,__FILE__,__LINE__)
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <vector>
extern "C" std::size_t qtvis_speex_allocation_calls();

namespace {
std::atomic<std::size_t> allocation_count{0};
using Clock=std::chrono::steady_clock;
using dsp_kernels::PreparedResampler;using dsp_kernels::ResamplerMode;using dsp_kernels::ResamplerConfig;
constexpr ResamplerMode modes[]={ResamplerMode::Bypass,ResamplerMode::ExistingFir,ResamplerMode::CompactFir,
    ResamplerMode::SpeexDsp,ResamplerMode::IirExperimental,ResamplerMode::LinearDiagnostic};
const char* name(ResamplerMode m){switch(m){case ResamplerMode::Bypass:return "bypass";case ResamplerMode::ExistingFir:return "existing_fir";
    case ResamplerMode::CompactFir:return "compact_fir";case ResamplerMode::SpeexDsp:return "speex";case ResamplerMode::IirExperimental:return "iir";default:return "linear";}}
double micros(Clock::time_point a,Clock::time_point b){return std::chrono::duration<double,std::micro>(b-a).count();}
double percentile(std::vector<double> v,double p){std::sort(v.begin(),v.end());return v[std::size_t(p*(v.size()-1))];}
const auto& factory=desktop_resampling::speex_backend_factory();
std::vector<float> signal(unsigned rate,std::size_t count,bool changing){
    std::vector<float> values(count);const double frequencies[]={230,900,2700,5300,9900,18000,20500};
    for(std::size_t i=0;i<count;++i){const double t=double(i)/rate;double v=0;
        for(std::size_t k=0;k<7;++k)v+=(changing?(.06+.02*std::sin(2.*std::numbers::pi*(k+1)*.7*t)):.08)*std::sin(2.*std::numbers::pi*frequencies[k]*t+.13*k);
        values[i]=float(v);}return values;
}
std::vector<float> run(PreparedResampler& op,const std::vector<float>& in,std::size_t packet,bool fragments){
    const auto& c=op.config();std::vector<float> result;result.reserve(PreparedResampler::maximum_output(in.size(),c.input_rate,c.output_rate));
    std::vector<float> output(PreparedResampler::maximum_output(packet,c.input_rate,c.output_rate));
    for(std::size_t at=0,step=0;at<in.size();++step){const auto count=std::min(in.size()-at,fragments?1+(step*37)%packet:packet);
        const auto bound=PreparedResampler::maximum_output(count,c.input_rate,c.output_rate);std::size_t written=0;
        ASSERT(op.input_contiguous(std::int64_t(at)));const auto before=op.produced();
        const auto allocations=allocation_count.load(),vendor_allocations=qtvis_speex_allocation_calls();ASSERT(op.process(std::span(in).subspan(at,count),count,count,output,bound,written));
        ASSERT(allocation_count.load()==allocations && qtvis_speex_allocation_calls()==vendor_allocations);ASSERT(op.produced()==before+written);ASSERT(op.advance_input(std::int64_t(at),count));
        result.insert(result.end(),output.begin(),output.begin()+written);at+=count;}
    return result;
}
double amplitude(const std::vector<float>& output,unsigned rate,double hz){
    const auto skip=std::min<std::size_t>(4096,output.size()/4);double re=0,im=0;
    for(std::size_t i=skip;i<output.size();++i){const double a=2.*std::numbers::pi*hz*i/rate;re+=output[i]*std::cos(a);im+=output[i]*std::sin(a);}
    return 2.*std::hypot(re,im)/(output.size()-skip);
}
double db(double value){return 20.*std::log10(std::max(1e-12,value));}
void resampler_state_replacement(const ResamplerConfig& c,const std::vector<float>& in,unsigned inrate,unsigned outrate){
    // Compatible Apply preserves rational phase, filter state and clock provenance.
    PreparedResampler source;ASSERT(source.prepare(c,&factory));const auto prefix=std::vector<float>(in.begin(),in.begin()+731);
    run(source,prefix,97,true);source.anchor(41);PreparedResampler copy(source),target;ASSERT(target.prepare(c,&factory));
    const auto allocations=allocation_count.load(),vendor_allocations=qtvis_speex_allocation_calls();ASSERT(target.copy_state_from(source));ASSERT(allocation_count.load()==allocations && qtvis_speex_allocation_calls()==vendor_allocations);
    const std::vector<float> suffix(in.begin()+731,in.end());
    // Offset continuity metadata is tested above; clear input expectations only
    // in the driver by feeding suffix directly while preserving DSP state.
    std::vector<float> x(PreparedResampler::maximum_output(suffix.size(),inrate,outrate)),y(x.size()),z(x.size());std::size_t nx=0,ny=0,nz=0;
    ASSERT(source.process(suffix,suffix.size(),suffix.size(),x,x.size(),nx));ASSERT(copy.process(suffix,suffix.size(),suffix.size(),y,y.size(),ny));
    ASSERT(target.process(suffix,suffix.size(),suffix.size(),z,z.size(),nz));ASSERT(nx==ny && ny==nz);
    for(std::size_t i=0;i<nx;++i){ASSERT(x[i]==y[i]);ASSERT(x[i]==z[i]);}
    source.reset();ASSERT(!source.anchored() && source.produced()==0 && source.input_contiguous(-900));
}
void numerical(ResamplerMode mode,unsigned inrate,unsigned requested){
    const unsigned outrate=mode==ResamplerMode::Bypass?inrate:requested;ResamplerConfig c{mode,inrate,outrate,1,5};
    PreparedResampler a,b;ASSERT(a.prepare(c,&factory));ASSERT(b.prepare(c,&factory));
    dsp_kernels::ResamplerResources query;ASSERT(PreparedResampler::resource_requirements(c,&factory,query));
    ASSERT(query.owned_bytes==a.resources().owned_bytes);ASSERT(query.fixed_delay_input_samples==a.resources().fixed_delay_input_samples);
    const auto in=signal(inrate,inrate,true);a.anchor(137);b.anchor(137);
    const auto whole=run(a,in,in.size(),false),fragmented=run(b,in,509,true);
    ASSERT(whole.size()==fragmented.size());ASSERT(whole.size()==outrate);
    for(std::size_t i=0;i<whole.size();++i)ASSERT(std::abs(whole[i]-fragmented[i])<2e-6f);
    ASSERT(a.anchor_ticks()==137 && a.produced()==whole.size());ASSERT(!a.input_contiguous(0));
    resampler_state_replacement(c,in,inrate,outrate);
    const auto stationary=signal(inrate,inrate*2,false);PreparedResampler response;ASSERT(response.prepare(c,&factory));const auto out=run(response,stationary,480,false);
    const double gain=db(amplitude(out,outrate,900)/.08),high=db(amplitude(out,outrate,9900)/.08);
    const double alias_frequency=std::abs(18000.-outrate*std::round(18000./outrate));
    const double alias=db(amplitude(out,outrate,alias_frequency)/.08);
    std::cout<<"{\"mode\":\""<<name(mode)<<"\",\"in\":"<<inrate<<",\"out\":"<<outrate<<",\"response_db\":[";
    bool first=true;for(double frequency:{230.,900.,2700.,5300.,9900.}){if(!first)std::cout<<',';first=false;std::cout<<db(amplitude(out,outrate,frequency)/.08);}std::cout<<"]}\n";
    ASSERT(std::abs(gain)<.25);if(mode==ResamplerMode::ExistingFir || mode==ResamplerMode::CompactFir || mode==ResamplerMode::SpeexDsp)ASSERT(std::abs(high)<.6);
    if(inrate!=outrate){if(mode==ResamplerMode::ExistingFir)ASSERT(alias<-40);if(mode==ResamplerMode::CompactFir)ASSERT(alias<-27);
        if(mode==ResamplerMode::SpeexDsp)ASSERT(alias<-45);if(mode==ResamplerMode::IirExperimental)ASSERT(alias<-8);}
    std::cout<<"{\"mode\":\""<<name(mode)<<"\",\"in\":"<<inrate<<",\"out\":"<<outrate<<",\"gain_900_db\":"<<gain
        <<",\"gain_9900_db\":"<<high<<",\"alias_18000_db\":"<<alias<<",\"owned_payload_bytes\":"<<query.owned_bytes
        <<",\"coefficient_bytes\":"<<query.coefficient_bytes<<",\"state_bytes\":"<<query.state_bytes<<",\"fixed_delay_input_samples\":"<<query.fixed_delay_input_samples
        <<",\"frequency_dependent_delay\":"<<query.frequency_dependent_delay<<"}\n";
}
void timing(ResamplerMode mode){const unsigned out=mode==ResamplerMode::Bypass?48000:32768;const ResamplerConfig c{mode,48000,out,1,5};
    std::vector<double> prepare_times,process_times;prepare_times.reserve(21);process_times.reserve(400);
    PreparedResampler op;for(unsigned k=0;k<21;++k){const auto start=Clock::now();ASSERT(op.prepare(c,&factory));prepare_times.push_back(micros(start,Clock::now()));}
    auto input=signal(48000,480,false);std::vector<float> output(PreparedResampler::maximum_output(input.size(),48000,out));
    for(unsigned k=0;k<420;++k){std::size_t written=0;const auto allocations=allocation_count.load(),vendor_allocations=qtvis_speex_allocation_calls();const auto start=Clock::now();
        ASSERT(op.process(input,input.size(),input.size(),output,output.size(),written));const auto elapsed=micros(start,Clock::now());ASSERT(allocations==allocation_count.load() && qtvis_speex_allocation_calls()==vendor_allocations);if(k>=20)process_times.push_back(elapsed);}
    std::cout<<"{\"mode\":\""<<name(mode)<<"\",\"packet_frames\":480,\"prepare_p50_us\":"<<percentile(prepare_times,.5)<<",\"prepare_p95_us\":"<<percentile(prepare_times,.95)
        <<",\"process_p50_us\":"<<percentile(process_times,.5)<<",\"process_p95_us\":"<<percentile(process_times,.95)<<"}\n";
}
}
void* operator new(std::size_t size){allocation_count.fetch_add(1);if(void* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
void identity_classification_oracles(){
    // IEEE754 classification must retain every finite bit pattern on identity
    // and reject both infinities and quiet/signaling NaNs atomically.
    for(const auto mode:modes){PreparedResampler identity;ASSERT(identity.prepare({mode,48000,48000,1,5},&factory));
        const std::array<std::uint32_t,8> patterns{0u,0x80000000u,1u,0x80000001u,0x7f7fffffu,0xff7fffffu,0x3f800000u,0xbf800000u};
        std::array<float,8> input{};std::array<float,9> out{};for(std::size_t i=0;i<patterns.size();++i)input[i]=std::bit_cast<float>(patterns[i]);
        std::size_t n=0;ASSERT(identity.process(input,input.size(),input.size(),out,out.size(),n));ASSERT(n==input.size());
        for(std::size_t i=0;i<n;++i)ASSERT(std::bit_cast<std::uint32_t>(out[i])==patterns[i]);
        for(const auto invalid:{0x7f800000u,0xff800000u,0x7fc00001u,0x7f800001u}){input.back()=std::bit_cast<float>(invalid);
            const auto before=identity.produced();out.fill(42.f);n=99;ASSERT(!identity.process(input,input.size(),input.size(),out,out.size(),n));
            ASSERT(n==0 && identity.produced()==before);for(const auto v:out)ASSERT(v==42.f);}
    }
}
void runtime_failure_oracles(){
    // Invalid input preserves the live state; arithmetic overflow invalidates
    // the entire wrapper, including its origin and continuity metadata.
    for(const auto mode:{ResamplerMode::ExistingFir,ResamplerMode::CompactFir}){
        PreparedResampler op;ASSERT(op.prepare({mode,48000,32000,1,5}));
        ASSERT(op.start_at(17,1));ASSERT(op.advance_input(25,1));
        std::vector<float> bad(400,NAN),output(PreparedResampler::maximum_output(bad.size(),48000,32000));
        std::size_t written=999;
        ASSERT(!op.process(bad,bad.size(),bad.size(),output,output.size(),written));
        ASSERT(written==0 && op.anchored() && op.anchor_ticks()==17 && op.input_contiguous(26));
        // Choose signs matching one prepared phase so its dot product exceeds
        // Float32 max even though every individual input is finite.
        dsp_kernels::PcmResampler coefficients;ASSERT(coefficients.prepare(48000,32000,1,
            mode==ResamplerMode::ExistingFir?dsp_kernels::PcmResampler::Quality::Full:dsp_kernels::PcmResampler::Quality::Compact));
        const auto g=coefficients.filter_geometry();const auto w=coefficients.coefficients();
        op.reset();ASSERT(op.start_at(17,0));ASSERT(op.advance_input(25,bad.size()));
        std::fill(bad.begin(),bad.end(),0.f);
        const std::size_t sample=3*((g.taps+2)/3);
        for(std::size_t k=0;k<g.taps;++k)bad[sample-k]=std::copysign(std::numeric_limits<float>::max(),w[k]);
        ASSERT(!op.process(bad,bad.size(),bad.size(),output,output.size(),written));
        ASSERT(written==0 && !op.anchored() && op.produced()==0 && op.input_contiguous(-900));
        ASSERT(op.start_at(-11,3));
    }
}
void ramp_and_quality_oracles(){
    // A ramp is an exact independent timestamp/linear interpolation oracle:
    // output m describes input time m*Fs_in/Fs_out, with no one-sample shift.
    for(unsigned inrate:{48000u,44100u})for(unsigned outrate:{32768u,32000u,36000u}){
        PreparedResampler linear;ASSERT(linear.prepare({ResamplerMode::LinearDiagnostic,inrate,outrate,1,5}));
        std::vector<float> ramp(1001);for(std::size_t i=0;i<ramp.size();++i)ramp[i]=float(i);
        const auto output=run(linear,ramp,97,true);ASSERT(linear.resources().fixed_delay_input_samples==0);
        for(std::size_t m=0;m<output.size();++m)ASSERT(std::abs(output[m]-double(m)*inrate/outrate)<8e-5);
    }
    for(unsigned quality=0;quality<=10;++quality){const ResamplerConfig c{ResamplerMode::SpeexDsp,44100,32768,1,quality};PreparedResampler op;
        dsp_kernels::ResamplerResources query;ASSERT(PreparedResampler::resource_requirements(c,&factory,query));ASSERT(op.prepare(c,&factory));ASSERT(query.owned_bytes==op.resources().owned_bytes);
        const auto in=signal(44100,1001,true);ASSERT(!run(op,in,97,true).empty());}
}
void independent_plane_oracles(){
    // Both planes retain separate filter state; graph nodes normally use one.
    for(const auto mode:modes){ResamplerConfig c{mode,48000,mode==ResamplerMode::Bypass?48000u:32768u,2,5};PreparedResampler op;ASSERT(op.prepare(c,&factory));
        const auto count=503u;const auto cap=PreparedResampler::maximum_output(count,c.input_rate,c.output_rate);std::vector<float> in(count*2),out(cap*2);
        for(std::size_t i=0;i<count;++i){in[i]=float(std::sin(.03*i));in[count+i]=-in[i];}std::size_t written=0;ASSERT(op.process(in,count,count,out,cap,written));
        for(std::size_t i=0;i<written;++i)ASSERT(std::abs(out[i]+out[cap+i])<1e-6f);op.reset();ASSERT(op.process(in,count,count,out,cap,written));
        for(std::size_t i=0;i<written;++i)ASSERT(std::abs(out[i]+out[cap+i])<1e-6f);}
}
int main(){
    identity_classification_oracles();
    runtime_failure_oracles();
    PreparedResampler unavailable;ASSERT(!unavailable.prepare({ResamplerMode::SpeexDsp,48000,32768,1,5}));
    ASSERT(!unavailable.prepare({ResamplerMode::Bypass,48000,32768,1,5}));
    ramp_and_quality_oracles();
    for(const auto mode:modes){for(unsigned in:{48000u,44100u})for(unsigned out:{32768u,32000u,36000u})numerical(mode,in,out);timing(mode);}
    independent_plane_oracles();
    std::cout<<"resampling_modes_tests PASS; RAM is owned object/heap payload, excludes allocator and driver staging; no RSS/native claim\n";
}
