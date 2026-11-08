// SPDX-License-Identifier: AGPL-3.0-or-later
#include <core_app/numeric_btrack_reference_graph.h>
#include <core_app/numeric_default_graph.h>
#include <domain/graph/node_factory.h>
#include <domain/graph/preparation_values.h>
#include <dataflow_runtime/runtime.h>
#include <dataflow_runtime/numeric_operation_binding.h>
#include <rhythm_graph_adapters/profile.h>
#include <rhythm_graph_adapters/btrack_composition.h>
#include <rhythm_reference_ports/predictive_beat_tracker.h>
#include <dsp_kernels/prepared_resampler.h>
#include <dsp_kernels/spectral_autocorrelation.h>
#include <rhythm_graph_adapters/btrack_ports.h>
#include <sinc_backend.h>
#include <kiss_backend.h>
#include <ooura_backend.h>
#include "numeric_test_builders.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>

namespace {
using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
namespace r=rhythm_reference_ports;
std::size_t allocations=0;
void check(bool value,const char* message){if(!value){std::fprintf(stderr,"BTrack FAIL: %s\n",message);std::abort();}}
constexpr FrameLimits limits{8,800000,3200000,65536,4096,65536};
NumericRuntimeProfile profile(bool sinc=true){
    NumericRuntimeProfile result{{64,128,16,64,1048576,16777216,limits},{{16777216,67108864,33554432,1048576},0,268435456},
        {800000,8,3200000,3200000,16777216},{true,true,true}};
    std::shared_ptr<const dsp_kernels::ResamplerBackendFactory> backend;
    if(sinc)backend={&desktop_resampling::sinc_backend_factory(),[](const auto*){}};
    result.fourier_backend={&desktop_fourier::kiss_backend_factory(),[](const auto*){}};
    return rhythm_graph_adapters::with_reference_operations(std::move(result),{},std::move(backend));
}
OwnedProcessingContract own(const NumericContract& contract){
    OwnedProcessingContract result;check(own_processing_contract(contract,limits,16777216,result)==Status::ok,"own contract");return result;
}
NumericProcessingGraph graph(std::uint32_t rate=44100){
    auto result=numeric_btrack_reference_example(rate).graph;
    std::erase_if(result.nodes,[](const auto& node){const auto kind=node_kind(node.kind);return kind && is_view(*kind);});
    std::erase_if(result.edges,[&](const auto& edge){return std::none_of(result.nodes.begin(),result.nodes.end(),[&](const auto& n){return n.id==edge.to;});});
    auto& source=result.nodes[0];const auto input=source.outputs[0].contract;source.kind="NetworkInput";
    source.parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t(rate)}}}};
    source.outputs={{0,PortDirection::Output,input}};
    check(attach_numeric_parameter_ports(source,profile().admission)==Status::ok,"explicit source Fs");return result;
}
void preparation_failure(const NumericProcessingGraph& input,const NumericRuntimeProfile& p,Status status){
    if(status==Status::ok)return;
    std::vector<std::size_t> workspace(input.nodes.size()*2);ProcessingGraphAdmission admitted;
    const auto capabilities=PreparedGraph::numeric_capabilities(p);
    const auto diagnostic=admit_processing_graph(input,p.admission,capabilities,workspace,admitted);
    std::fprintf(stderr,"BTrack prepare status=%u admission=%u node=%d port=%u reason=%u peak=%zu\n",
        unsigned(status),unsigned(diagnostic.error),int(diagnostic.node),unsigned(diagnostic.port),unsigned(diagnostic.reason),admitted.peak_bytes);
    for(const auto& node:input.nodes){
        ResolvedProcessingNode resolved;const auto result=resolve_processing_node(node,p.admission,resolved);
        if(!result || !capabilities.query(capabilities.context,resolved,capabilities.profile).supported)
            std::fprintf(stderr,"BTrack unsupported node=%d kind=%s domain_error=%u\n",int(node.id),node.kind.c_str(),unsigned(result.error));
    }
}
double scalar(const NumericView& value){
    if(value.contract->dtype==DType::Float64){double result=0;check(load_element(value,{0,0,0},result)==Status::ok,"Double scalar value");return result;}
    float result=0;check(load_element(value,{0,0,0},result)==Status::ok,"scalar value");return result;
}
bool close(float actual,float expected){return std::abs(actual-expected)<=.002f+.0005f*std::abs(expected);}
void compare(const NumericView& actual,std::span<const float> expected,const char* message){
    check(actual.actual[0]==expected.size(),"vector extent");
    for(std::size_t i=0;i<expected.size();++i){float value=0;check(load_element(actual,{0,i,0},value)==Status::ok && close(value,expected[i]),message);}
}
struct Trace {
    std::vector<double> onset;
    std::vector<float> state,history,src,comb,observations,probabilities,magnitude,phase;
    std::vector<std::uint64_t> beats;
    explicit Trace(std::size_t count):onset(count),state(count*3),history(count*512),src(count*512),
        comb(count*128),observations(count*41),probabilities(count*41),magnitude(count*1024),phase(count*1024),beats(count){}
    static void copy(const NumericView& value,std::span<float> output){
        for(std::size_t i=0;i<output.size();++i)check(load_element(value,{0,i,0},output[i])==Status::ok,"copy diagnostic");
    }
    void capture(NodeId id,PortId port,const NumericView& value,std::size_t step){
        if(id==26 && port==0)copy(value,std::span(magnitude).subspan(step*1024,1024));
        if(id==26 && port==1)copy(value,std::span(phase).subspan(step*1024,1024));
        if(id==30)onset[step]=scalar(value);
        if(id==31 && port==1)copy(value,std::span(history).subspan(step*512,512));
        if(id==32)copy(value,std::span(src).subspan(step*512,512));
        if(id==33 && port==0)copy(value,std::span(observations).subspan(step*41,41));
        if(id==85)copy(value,std::span(comb).subspan(step*128,128));
        if(id==34 && port==0)beats[step]=std::uint64_t(scalar(value));
        if(id==34 && port==1)state[3*step]=scalar(value);
        if(id==34 && port==3)state[3*step+1]=scalar(value);
        if(id==34 && port==2)state[3*step+2]=scalar(value);
        if(id==34 && port==7)copy(value,std::span(probabilities).subspan(step*41,41));
    }
};
struct Observer {
    r::PreparedCombTempoTransition tempo;
    r::PreparedBeatPrediction prediction;
    r::BeatPredictionObservation expected;
    r::ScalarHistory history;
    dsp_kernels::PreparedResampler resampler;
    dsp_kernels::PreparedSpectralAutocorrelation correlation;
    std::array<float,512> threshold{},acf{},resampled{};
    Trace trace;
    std::size_t hops=0,decisions=0,beats=0,markers=0;
    bool allow_markers=false;
    explicit Observer(std::size_t count):trace(count){
        check(tempo.prepare(512) && prediction.prepare(512,43,5,.9f) && history.prepare(512),"prepare independent atomic state");
        for(std::size_t i=0;i<512;++i)history.set(i,i%43==0?1.f:0.f);
        dsp_kernels::ResamplingWindowConfig sinc{512,512,1,dsp_kernels::ResamplingWindowMode::SincBestQuality,1,dsp_kernels::NumericalPrecision::DesktopReference};
        check(resampler.prepare_window(sinc,&desktop_resampling::sinc_backend_factory()),"prepare independent sinc");
        dsp_kernels::SpectralAutocorrelationConfig config;config.fft_size=1024;
        config.normalization=dsp_kernels::CorrelationNormalization::Unbiased;
        config.inverse_output=dsp_kernels::AutocorrelationInverseOutput::ComplexMagnitude;
        config.transform=dsp_kernels::AutocorrelationTransform::Complex;
        check(correlation.prepare(512,config),"prepare shared ACF oracle");
    }
    void hop(const NumericView&){++hops;}
    void history_value(const NumericView& value){
        const auto current=float(scalar(value));
        check(current==float(std::abs(trace.onset[hops-1])+.0001),"explicit abs/add/converter source rounding");
        history.push(current);check(prediction.process(current,expected),"independent prediction");
    }
    void history_window(const NumericView& value){
        compare(value,history.values(),"seeded ODF history");std::size_t written=0;
        check(resampler.process_window(history.values(),512,resampled,512,written) && written==512,"independent whole-window sinc");
    }
    void observations(const NumericView& value){
        if(!expected.beat_due)return;
        std::array<float,41> values{};Trace::copy(value,values);float period=0;
        check(tempo.advance_observations(values,period) && prediction.set_period(period),"tempo advances only on predicted beats");
        expected.period_frames=period;
    }
    void decision(PortId port,const NumericView& value){
        using P=rhythm_graph_adapters::btrack_ports::DecisionOutput;
        if(port==PortId(P::BeatFlag)){check(scalar(value)==float(expected.beat_due),"beat scheduling");++decisions;beats+=expected.beat_due;}
        if(port==PortId(P::TempoBpm))check(close(scalar(value),tempo.bpm()),"BPM");
        if(port==PortId(P::PeriodFrames))check(scalar(value)==expected.period_frames,"integer period");
        if(port==PortId(P::CumulativeScore))check(close(scalar(value),prediction.latest_cumulative_score()),"cumulative score");
        if(port==PortId(P::CumulativeHistory))compare(value,prediction.cumulative_scores(),"cumulative history");
        if(port==PortId(P::TempoProbabilities))compare(value,tempo.state_probabilities(),"tempo state advances only on beats");
        if(port==PortId(P::NextBeat) || port==PortId(P::NextPrediction)){std::int64_t result=0;check(load_element(value,{0,0,0},result)==Status::ok &&
            result==(port==PortId(P::NextBeat)?expected.next_beat:expected.next_prediction),"exact predictor counters");}
    }
    void stage(NodeId id,PortId port,const NumericView& value){
        if(id==31 && port==0)history_value(value);
        if(id==31 && port==1)history_window(value);
        if(id==32)compare(value,resampled,"same-input sinc on every hop");
        if(id==70){Trace::copy(value,threshold);check(correlation.process(threshold,acf),"independent shared ACF");}
        if(id==83)compare(value,acf,"explicit Fourier/power/IDFT/hypot/division equals shared ACF");
        if(id==33)observations(value);
        if(id==34)decision(port,value);
    }
    void observe(const NumericPortFrameView& event){
        if(allow_markers && !hops && event.status==Status::unavailable){++markers;return;}
        check(event.status==Status::ok,"every graph stage available");const auto& value=std::get<NumericView>(event.frame.payload);
        const auto id=event.identity.node;const auto port=event.identity.port;
        if(id==3)hop(value);
        if(id==26 || (id>=30 && id<=34) || id==85){check(hops && hops<=trace.onset.size(),"bounded trace");trace.capture(id,port,value,hops-1);}
        stage(id,port,value);
    }
    static void output(void* user,const NumericPortFrameView& event){static_cast<Observer*>(user)->observe(event);}
};
std::vector<float> signal(bool silence){
    std::vector<float> result(512*1024);if(silence)return result;
    constexpr float tones[]{110,251,503,997,1999,4001,6007,10007};
    for(std::size_t i=0;i<result.size();++i){
        const double seconds=double(i)/44100,phase=double(i%22050)/44100;
        for(unsigned k=0;k<8;++k)result[i]+=float(.06*std::exp(-phase/.006)*
            std::sin(6.283185307179586*tones[k]*seconds+.17*k+.06*std::sin(seconds*(k+1))));
    }
    return result;
}
void feed(const NumericProcessingGraph& source,const PreparedGraph& plan,RuntimeContext& state,std::span<const float> pcm,Observer& observer){
    const auto data=source.nodes[0].outputs[0].contract.view();const auto& contract=std::get<NumericContract>(data);
    const PlaneLayout plane{0,{sizeof(float),0}};const auto before=allocations;
    for(std::size_t at=0;at+512<=pcm.size();at+=512){
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=at/512+1;stamp.producer_epoch=1;stamp.has_sample_interval=true;
        stamp.sample_begin=std::int64_t(at);stamp.sample_count=512;stamp.timestamp_ticks=std::int64_t(at);
        const auto frame=numeric_test_builders::frame(stamp,contract,std::as_bytes(pcm.subspan(at,512)),std::span(&plane,1),{512,1});
        check(plan.process_numeric_ports(1,0,frame,state,{},NumericRuntimeCallbacks{Observer::output,nullptr,nullptr,&observer})==Status::ok,"feed source");
    }
    check(allocations==before,"all graph stages and reference processing allocate no C++ heap");
}
NumericProcessingGraph variable_graph(){
    auto result=graph();auto contract=std::get<NumericContract>(result.nodes[0].outputs[0].contract.view());
    contract.shape.extents[0]={1,8192};result.nodes[0].outputs[0].contract=own(contract);
    check(bool(configure_processing_input(result.nodes[1],contract,profile().admission)),"variable PCM resampler input");
    const auto resampled=result.nodes[1].outputs[0].contract.view();
    check(bool(configure_processing_input(result.nodes[2],resampled,profile().admission)),"variable packets to fixed-hop framer");
    return result;
}
void feed_chunks(const NumericProcessingGraph& source,const PreparedGraph& plan,RuntimeContext& state,
    std::span<const float> pcm,Observer& observer,std::uint64_t epoch,std::size_t chunk){
    const auto data=source.nodes[0].outputs[0].contract.view();const auto& contract=std::get<NumericContract>(data);
    const PlaneLayout plane{0,{sizeof(float),0}};std::array<float,8192> packet{};
    const auto before=allocations;std::uint64_t sequence=0;
    for(std::size_t at=0;at<pcm.size();){
        const auto count=std::min(chunk,pcm.size()-at);FrameStamp stamp;
        stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=epoch;stamp.has_sample_interval=true;
        // PCM ingress timestamps identify the first sample, as required by the
        // existing resampler's anchor/advance_input and resampler runtime tests.
        stamp.sample_begin=std::int64_t(at);stamp.sample_count=count;stamp.timestamp_ticks=std::int64_t(at);
        std::copy_n(pcm.begin()+at,count,packet.begin());
        const auto frame=numeric_test_builders::frame(stamp,contract,std::as_bytes(std::span(packet)),std::span(&plane,1),{count,1});
        const auto status=plan.process_numeric_ports(1,0,frame,state,{},NumericRuntimeCallbacks{Observer::output,nullptr,nullptr,&observer});
        if(status!=Status::ok)std::fprintf(stderr,"BTrack chunk status=%u epoch=%llu offset=%zu count=%zu hops=%zu decisions=%zu\n",
            unsigned(status),(unsigned long long)epoch,at,count,observer.hops,observer.decisions);
        check(status==Status::ok,"feed partition or new epoch");at+=count;
    }
    check(allocations==before,"partition/reset path has no C++ heap allocation");
}
void chunks_and_epoch(){
    const auto input=variable_graph();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    const auto p=profile();const auto status=PreparedGraph::prepare_numeric(input,p,plan,state);preparation_failure(input,p,status);
    check(status==Status::ok,"prepare variable packet graph");
    const auto pcm=signal(false);Observer first{pcm.size()/512},second{pcm.size()/512};
    // Before the first complete hop, warm-up/reset markers carry no DSP frame.
    // Once a hop is published every subscribed stage must remain available.
    first.allow_markers=second.allow_markers=true;
    feed_chunks(input,*plan,*state,pcm,first,1,97);
    feed_chunks(input,*plan,*state,pcm,second,2,8192);
    check(first.hops==1024 && second.hops==1024 && first.decisions==second.decisions,"exact hop count for97/8192 samples");
    check(first.trace.onset==second.trace.onset && first.trace.state==second.trace.state && first.trace.beats==second.trace.beats,
        "packet partition and producer reset reproduce fresh canonical state");
}
template<class T> void save(const std::string& prefix,const char* suffix,std::span<const T> values){
    std::ofstream file(prefix+suffix,std::ios::binary);file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size_bytes()));
    check(bool(file),"write evidence");
}
void save_trace(const std::string& prefix,std::span<const float> pcm,const Trace& trace){
    save<float>(prefix,".pcm.f32",pcm);save<double>(prefix,".odf.f64",trace.onset);save<float>(prefix,".state.f32",trace.state);
    save<float>(prefix,".norm.f32",trace.magnitude);save<float>(prefix,".phase.f32",trace.phase);
    save<float>(prefix,".history.f32",trace.history);save<float>(prefix,".resampled.f32",trace.src);save<float>(prefix,".comb.f32",trace.comb);
    save<float>(prefix,".observations.f32",trace.observations);save<float>(prefix,".delta.f32",trace.probabilities);save<std::uint64_t>(prefix,".beat.u64",trace.beats);
}
void live(bool silence,const char* prefix){
    const auto input=graph();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    const auto p=profile();const auto status=PreparedGraph::prepare_numeric(input,p,plan,state);preparation_failure(input,p,status);
    check(status==Status::ok,"prepare modular BTrack graph");
    const auto pcm=signal(silence);Observer observer{pcm.size()/512};feed(input,*plan,*state,pcm,observer);
    check(observer.hops==pcm.size()/512 && observer.decisions==observer.hops,"one ODF/decision per fixed hop");
    if(!silence)check(observer.beats>10,"nonempty synthetic beat evidence");
    if(prefix)save_trace(prefix,pcm,observer.trace);
    std::printf("BTrack silence=%d hops=%zu beats=%zu\n",int(silence),observer.hops,observer.beats);
}
void absent_provider(){
    const auto input=graph();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(input,profile(false),plan,state)!=Status::ok,"missing sinc rejects atomically even at ratio1");
}
void fourier_provider_admission(){
    auto input=graph();auto p=profile();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    p.fourier_backend.reset();
    check(PreparedGraph::prepare_numeric(input,p,plan,state)!=Status::ok && !plan && !state,"named FFT provider missing rejects before publication");
    p.fourier_backend={&desktop_fourier::ooura_backend_factory(),[](const auto*){}};
    check(PreparedGraph::prepare_numeric(input,p,plan,state)!=Status::ok && !plan && !state,"mismatched FFT provider does not silently fallback");
    const auto node=std::find_if(input.nodes.begin(),input.nodes.end(),[](const auto& value){return value.id==25;});
    auto& parameters=std::get<TransformProcessingParameters>(node->parameters).values;
    const auto field=std::find_if(parameters.begin(),parameters.end(),[](const auto& value){return value.key=="backend_id";});
    check(field!=parameters.end(),"explicit FFT selection setting");field->value=std::string{};p.fourier_backend.reset();
    const auto status=PreparedGraph::prepare_numeric(input,p,plan,state);preparation_failure(input,p,status);
    check(status==Status::ok,"empty backend selection preserves built-in Fourier without provider");
}
void split_comb(){
    r::PreparedCombTempoTransition transition;check(transition.prepare(512),"prepare atomic tempo state");
    check(transition.workspace_bytes()==0,"tempo transition owns no hidden Fourier plan");
    std::array<float,512> onset{},threshold{};std::array<float,128> response{},clean{};std::array<float,41> observations{};
    r::PreparedBTrackCombResponse comb;check(comb.prepare(),"prepare atomic comb response");
    onset.fill(2);const auto before=allocations;
    check(r::btrack_adaptive_threshold(onset,threshold),"standalone onset threshold");
    check(std::all_of(threshold.begin(),threshold.end(),[](float v){return v==0;}),"constant baseline removed");
    check(comb.process(threshold,response) && r::btrack_adaptive_threshold(response,clean) && r::btrack_comb_observations(clean,observations),"explicit pure observation stages");
    float period=0;check(transition.advance_observations(observations,period) && period>0,"zero observations retain source tie selection");
    check(allocations==before,"atomic stages allocate no hot memory");
}
struct HistoryStage {
    ConfiguredProcessingNode node;
    NumericContract input_contract;
    NumericBufferContract input_buffer_contract;
    std::array<NumericBufferContract,2> output_contracts;
    std::array<PlaneLayout,1> plane{{{0,{sizeof(float),0}}}};
    std::array<std::array<float,512>,2> storage{};
    std::array<NumericOperationOutput,2> outputs;
    std::unique_ptr<PreparedNumericOperation> operation;
    HistoryStage(){
        static constexpr std::uint32_t channels[]{0};input_contract.channel_order=channels;
        input_contract.dtype=DType::Float32;input_contract.clock={"btrack-test",{1,44100},44100};
        const std::array inputs{ProcessingPort{0,PortDirection::Input,own(input_contract)}};
        check(rhythm_graph_adapters::make_btrack_operation_node(10,"BTrackHistory",inputs,{},limits,65536,node)==Status::ok,"compose source history stage");
        const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::NumericExtension,node.inputs,node.outputs,&node.parameters,true};
        const auto p=profile();std::shared_ptr<const NumericOperationDescriptor> descriptor;
        check(p.operation_factory->describe(numeric_operation_node_view(resolved),descriptor)==Status::ok &&
            p.operation_factory->prepare(descriptor,numeric_operation_profile(p),operation)==Status::ok,"prepare source history stage");
        const auto physical=numeric_operation_node(node);input_buffer_contract=physical.inputs[0].contract;
        for(std::size_t i=0;i<2;++i){
            output_contracts[i]=physical.outputs[i].contract;
            outputs[i]={PortId(i),{&output_contracts[i],std::as_writable_bytes(std::span(storage[i])),plane,{i?512u:1u,1}}};
        }
    }
    Status process(PreparedNumericOperation& processor,float value,bool enabled=true){
        const NumericBufferView view{&input_buffer_contract,std::as_bytes(std::span(&value,1)),plane,{1,1}};
        const NumericOperationInput input{0,view,{},Status::ok,std::span(&value,1)};
        return processor.process(std::span(&input,1),outputs,{NumericOperationEvent::Continue,false,enabled});
    }
};
void history_lifecycle(){
    HistoryStage stage;check(stage.process(*stage.operation,3)==Status::ok,"initial history sample");
    auto clone=stage.operation->clone();check(bool(clone),"cold independent clone");const auto before=allocations;
    check(stage.process(*stage.operation,5)==Status::ok && stage.process(*clone,7)==Status::ok,"advance distinct branches");
    check(stage.storage[1][510]==3.f && stage.storage[1][511]==7.f,"clone owns original historical values");
    check(stage.process(*clone,std::numeric_limits<float>::quiet_NaN())!=Status::ok,"nonfinite input rejected");
    check(stage.outputs[0].status==Status::unavailable && stage.outputs[1].status==Status::unavailable,"no stale publication");
    check(stage.process(*clone,9)==Status::ok && stage.storage[1][510]==7.f,"invalid input preserved state");
    check(stage.process(*clone,4,false)==Status::ok,"disabled typed zeros");
    check(std::all_of(stage.storage[1].begin(),stage.storage[1].end(),[](float x){return x==0;}),"disabled history output zero");
    check(stage.process(*clone,8)==Status::ok && stage.storage[1][510]==9.f,"disabled processing does not advance history");
    check(clone->copy_state_from(*stage.operation)==Status::incompatible_contract,"explicit reset on Apply");
    check(stage.process(*clone,2)==Status::ok && stage.storage[1][510]==0 && stage.storage[1][511]==2.f,"reset reseeds source history");
    check(allocations==before,"history lifecycle uses no hot allocation");
}
}
void* operator new(std::size_t count){++allocations;if(auto* pointer=std::malloc(count?count:1))return pointer;throw std::bad_alloc();}
void operator delete(void* pointer) noexcept{std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept{std::free(pointer);}
int main(int argc,char** argv){split_comb();history_lifecycle();absent_provider();fourier_provider_admission();live(false,argc>1?argv[1]:nullptr);live(true,nullptr);chunks_and_epoch();std::puts("BTrack modular pipeline PASS");}
