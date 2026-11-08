// SPDX-License-Identifier: AGPL-3.0-or-later
#include <core_app/numeric_reference_graphs.h>
#include <rhythm_graph_adapters/profile.h>
#include <rhythm_graph_adapters/stage_ports.h>
#include <rhythm_reference_ports/context_beat_sequence.h>
#include <rhythm_reference_ports/onset_descriptor.h>
#include <rhythm_reference_ports/adaptive_peak_picker.h>
#include <rhythm_reference_ports/onset_decision.h>
#include <dsp_kernels/log_compression.h>
#include <domain/graph/node_factory.h>
#include "numeric_test_builders.h"
#include "aubio_reference_trace.h"
#include "aubio_context_test_inputs.h"
#include <ooura_backend.h>
#include <kiss_backend.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <malloc.h>

namespace {
using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
using namespace rhythm_graph_adapters;
std::size_t allocations=0;
void check(bool value,const char* why){if(!value){std::fprintf(stderr,"Aubio pipeline: %s\n",why);std::exit(1);}}
NumericRuntimeProfile profile() {
    NumericRuntimeProfile result{{256,1024,32,64,1048576,16777216,{8,800000,3200000,65536,4096,65536}},
        {{33554432,67108864,67108864,1048576},0,536870912},{800000,8,3200000,3200000,16777216},{true,true,true}};
    result.fourier_backend={&desktop_fourier::ooura_backend_factory(),[](const auto*){}};
    return rhythm_graph_adapters::with_reference_operations(std::move(result));
}
NumericProcessingGraph graph() {
    auto result=numeric_aubio_reference_example().graph;
    std::erase_if(result.nodes,[](const auto& node){const auto kind=node_kind(node.kind);return node.id==2 || (kind && is_view(*kind));});
    std::erase_if(result.edges,[&](const auto& edge){return edge.from==2 || edge.to==2 || std::none_of(result.nodes.begin(),result.nodes.end(),[&](const auto& n){return n.id==edge.to;});});
    auto& source=result.nodes[0];source.kind="NetworkInput";
    source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{44100}}}}};
    auto contract=std::get<NumericContract>(source.outputs[0].contract.view());contract.shape.extents[0]={1,8192};
    OwnedProcessingContract owned;check(own_processing_contract(contract,profile().admission.frame,16777216,owned)==Status::ok,"source contract");
    source.outputs[0].contract=std::move(owned);result.edges.push_back({1,3,0,0});
    result.nodes[1].inputs[0].contract=source.outputs[0].contract;
    return result;
}
struct Seen {
    rhythm_reference_ports::PreparedOnsetDescriptor onset_descriptor,tempo_descriptor;
    rhythm_reference_ports::PreparedAdaptivePeakPicker onset_picker,tempo_picker;
    aubio_test::PeakInputs onset_smoothing,tempo_smoothing;
    rhythm_reference_ports::PreparedOnsetDecision onset;
    rhythm_reference_ports::PreparedContextBeatSequence tempo;
    aubio_test::ContextInputs context_inputs;
    rhythm_reference_ports::AdaptivePeakObservation expected_onset_peak,expected_tempo_peak;
    rhythm_reference_ports::OnsetDecision expected_onset;
    rhythm_reference_ports::ContextBeatFrame expected_tempo;
    float expected_hfc=0,expected_flux=0;
    bool onset_silent=false,tempo_silent=false;
    std::array<float,513> magnitude{},compressed{};
    std::array<float,512> hop{};std::size_t frames=0,onsets=0,beats=0;
    unsigned tatum=0;aubio_trace::Trace* trace=nullptr;
    Seen(){
        using namespace rhythm_reference_ports;
        AdaptivePeakConfig onset_config;onset_config.threshold=.058f;
        AdaptivePeakConfig tempo_config;tempo_config.threshold=.3f;
        check(onset_descriptor.prepare(513,OnsetRecipe::FrequencyContent) &&
            tempo_descriptor.prepare(513,OnsetRecipe::SpectralFlux),"atomic source descriptors prepare");
        check(onset_picker.prepare(onset_config) && tempo_picker.prepare(tempo_config),"atomic source pickers prepare");
        check(onset_smoothing.prepare() && tempo_smoothing.prepare(),"explicit recursive smoothing prepare");
        ContextBeatSequenceConfig context;context.tracker.window=512;
        check(onset.prepare() && tempo.prepare(context) && context_inputs.prepare(),"atomic source decisions prepare");
    }
    void expected(const NumericView& value) {
        for(std::size_t i=0;i<hop.size();++i)check(load_element(value,{0,i,0},hop[i])==Status::ok,"PCM hop read");
        check(rhythm_reference_ports::pcm_is_silent(hop,-70,onset_silent) &&
            rhythm_reference_ports::pcm_is_silent(hop,-90,tempo_silent),"explicit silence reductions");
        ++frames;
    }
    void expected_spectrum(const NumericView& value) {
        for(std::size_t i=0;i<magnitude.size();++i)
            check(load_element(value,{0,i,0},magnitude[i])==Status::ok,"magnitude frame read");
        check(dsp_kernels::logarithmic_magnitude(magnitude,compressed,1),"explicit log compression");
        check(onset_descriptor.process(compressed,{},expected_hfc) &&
            tempo_descriptor.process(magnitude,{},expected_flux),"atomic descriptor processing");
        check(onset_smoothing.push(expected_hfc) && tempo_smoothing.push(expected_flux),"explicit forward/reverse smoothing");
        check(onset_picker.process(onset_smoothing.smoothed,expected_onset_peak) &&
            tempo_picker.process(tempo_smoothing.smoothed,expected_tempo_peak),"atomic peak processing");
        check(context_inputs.push(expected_tempo_peak.thresholded),"explicit previous history and context math");
        check(onset.process(expected_onset_peak.position,onset_silent,expected_onset) &&
            tempo.process(context_inputs.reversed,context_inputs.acf,tempo_silent,expected_tempo),"atomic decisions processing");
        tatum=tempo.poll_tatum();
    }
    void near(const NumericView& value,float expected,const char* why) {
        float actual=0;check(load_element(value,{0,0,0},actual)==Status::ok,why);
        if(std::isnan(expected)){check(std::isnan(actual),why);return;}
        if(std::isinf(expected)){check(actual==expected,why);return;}
        if(std::abs(actual-expected)>1e-3f+2e-5f*std::abs(expected))std::fprintf(stderr,"frame=%zu actual=%.9g expected=%.9g %s\n",frames,actual,expected,why);
        check(std::abs(actual-expected)<=1e-3f+2e-5f*std::abs(expected),why);
    }
    void scalar(NodeId node,PortId port,const NumericView& value) {
        if(node==21)near(value,expected_hfc,"HFC descriptor parity");
        if(node==30)near(value,expected_flux,"flux descriptor parity");
        if(node==22 && port==peak_picker_ports::thresholded)near(value,expected_onset_peak.thresholded,"onset picker parity");
        if(node==31 && port==peak_picker_ports::thresholded)near(value,expected_tempo_peak.thresholded,"tempo picker parity");
        if(node==24 && port==onset_decision_ports::detected){std::uint8_t v=0;check(load_element(value,{0,0,0},v)==Status::ok && bool(v)==expected_onset.detected,"onset event parity");++onsets;}
        if(node==24 && port==onset_decision_ports::last_sample){std::uint64_t v=0;check(load_element(value,{0,0,0},v)==Status::ok && v==expected_onset.sample,"onset exact sample parity");}
    }
    void tracking(PortId port,const NumericView& value) {
        if(port==context_sequence_ports::tactus)near(value,expected_tempo.source_tactus,"source tactus parity");
        if(port==context_sequence_ports::bpm){near(value,expected_tempo.bpm,"BPM parity");++beats;}
        if(port==context_sequence_ports::confidence)near(value,expected_tempo.confidence,"confidence parity");
        if(port==context_sequence_ports::beat_sample){std::int64_t v=0;check(load_element(value,{0,0,0},v)==Status::ok && v==expected_tempo.beat_sample,"beat sample parity");}
        if(port==context_sequence_ports::tatum_event){std::uint32_t v=0;check(load_element(value,{0,0,0},v)==Status::ok && v==tatum,"tatum polling parity");}
    }
    static void output(void* user,const NumericPortFrameView& frame) {
        auto& self=*static_cast<Seen*>(user);if(frame.status==Status::unavailable)return;
        check(frame.status==Status::ok,"available graph outputs have no errors");
        const auto& value=std::get<NumericView>(frame.frame.payload);const auto node=frame.identity.node;const auto port=frame.identity.port;
        if(node==3){self.expected(value);if(self.trace)self.trace->frame=self.frames;}
        if(node==11 && port==0)self.expected_spectrum(value);
        if(self.trace)self.trace->capture(node,port,value);
        if(node==33)self.tracking(port,value);else self.scalar(node,port,value);
    }
};
std::vector<float> signal(bool silent) {
    std::vector<float> result(44100*16);if(silent)return result;
    constexpr std::array tones{113.,229.,467.,941.,1889.,3797.,7603.};
    for(std::size_t i=0;i<result.size();++i){const double t=double(i)/44100,envelope=std::exp(-double(i%22050)/265.);
        for(std::size_t k=0;k<tones.size();++k)result[i]+=float(.07*envelope*std::sin(6.283185307179586*(tones[k]*t+.025*t*t*(k+1))));}
    return result;
}
void feed(const NumericProcessingGraph& graph,const PreparedGraph& plan,RuntimeContext& state,std::span<const float> pcm,std::size_t chunk,Seen& seen,std::uint64_t epoch=1) {
    const auto contract=std::get<NumericContract>(graph.nodes[0].outputs[0].contract.view());const PlaneLayout layout{0,{4,0}};
    std::array<float,8192> packet{};const auto before=allocations;
    for(std::size_t offset=0,sequence=0;offset<pcm.size();offset+=chunk){
        const auto count=std::min(chunk,pcm.size()-offset);FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=epoch;
        stamp.has_sample_interval=true;stamp.sample_begin=std::int64_t(offset);stamp.sample_count=count;stamp.timestamp_ticks=std::int64_t(offset);
        std::copy_n(pcm.begin()+offset,count,packet.begin());
        const auto frame=numeric_test_builders::frame(stamp,contract,std::as_bytes(std::span(packet)),std::span(&layout,1),{count,1});
        check(plan.process_numeric_ports(1,0,frame,state,{},NumericRuntimeCallbacks{Seen::output,nullptr,nullptr,&seen})==Status::ok,"graph ingest");
    }
    check(allocations==before,"no graph/native processing allocations");
}
void case_run(bool silent,std::size_t chunk,const char* directory=nullptr) {
    const auto source=graph();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(source,profile(),plan,state)==Status::ok,"full graph prepare");
    const auto pcm=signal(silent);Seen seen;aubio_trace::Trace trace(directory?pcm.size()/512:0);seen.trace=directory?&trace:nullptr;
    feed(source,*plan,*state,pcm,chunk,seen);
    check(seen.frames==pcm.size()/512 && seen.beats==seen.frames && seen.onsets==seen.frames,"one result per complete hop");
    if(directory)trace.export_values(directory,std::span(pcm).first(seen.frames*512));
    Seen reset;feed(source,*plan,*state,pcm,chunk,reset,2);
    check(reset.frames==seen.frames && reset.beats==seen.beats && reset.onsets==seen.onsets,"producer epoch resets complete recipe");
}
void provider_admission() {
    auto source=graph();auto selected=profile();
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    selected.fourier_backend.reset();
    check(PreparedGraph::prepare_numeric(source,selected,plan,state)!=Status::ok && !plan && !state,"named Ooura requires a provider");
    selected.fourier_backend={&desktop_fourier::kiss_backend_factory(),[](const auto*){}};
    check(PreparedGraph::prepare_numeric(source,selected,plan,state)!=Status::ok && !plan && !state,"KISS cannot replace named Ooura");
    for(auto& node:source.nodes)if(node.id==10)
        for(auto& parameter:std::get<TransformProcessingParameters>(node.parameters).values)
            if(parameter.key=="backend_id")parameter.value=std::string{};
    selected.fourier_backend.reset();
    check(PreparedGraph::prepare_numeric(source,selected,plan,state)==Status::ok,"explicit empty backend keeps builtin admission");
}
}
void* operator new(std::size_t n){++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void* operator new(std::size_t n,std::align_val_t align){++allocations;if(auto* p=_aligned_malloc(n?n:1,std::size_t(align)))return p;throw std::bad_alloc();}
void operator delete(void* p,std::align_val_t) noexcept{_aligned_free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{_aligned_free(p);}
void* operator new[](std::size_t n,std::align_val_t align){return ::operator new(n,align);}
void operator delete[](void* p,std::align_val_t align) noexcept{::operator delete(p,align);}
void operator delete[](void* p,std::size_t,std::align_val_t align) noexcept{::operator delete(p,align);}
int main(int argc,char** argv){provider_admission();case_run(false,512,argc>1?argv[1]:nullptr);case_run(false,777);case_run(true,4096);std::puts("aubio graph/native parity PASS");}
