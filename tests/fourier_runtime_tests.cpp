#include <dataflow_runtime/runtime.h>
#include "numeric_test_builders.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
using namespace signal_contracts;using namespace domain;using namespace dataflow_runtime;
constexpr std::size_t N=512;
constexpr std::array<std::uint32_t,1> mono{0};
constexpr FrameLimits bounds{1,32768,262144,16384,256,32768};
void check(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL Fourier runtime: %s\n",message);std::exit(1);}}
NumericRuntimeProfile profile(){auto p=NumericRuntimeProfile{{32,64,8,32,1048576,4*1048576,bounds},{{4*1048576,4*1048576,4*1048576,131072},0,32*1048576},{32768,1,262144,262144,1048576},{true,true,true}};p.music_enabled=true;return p;}
ProcessingPort port(PortDirection side,PortId id,const NumericContract& value){return numeric_test_builders::port(id,side,value,bounds,65536,check,"own port");}
NumericProcessingGraph graph(bool real,bool imaginaryConnected=true){
    NumericContract input;input.channel_order=mono;input.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{N,N},ExtentBounds{}}};input.clock={"fixture",{1,48000},48000};
    input.representation.alignment=alignof(float);
    ConfiguredProcessingNode source;source.id=1;source.kind="AudioSource";source.outputs={port(PortDirection::Output,0,input),port(PortDirection::Output,1,input)};
    ConfiguredProcessingNode fft;fft.id=2;fft.kind=real?"RealFourier":"Fourier";fft.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t(N)}}}};
    fft.inputs={port(PortDirection::Input,0,input)};if(!real)fft.inputs.push_back(port(PortDirection::Input,1,input));
    std::array<OwnedProcessingContract,4> output;check(fourier_output_contracts(fft,input,output)==Status::ok,"derive Fourier outputs");
    for(PortId p=0;p<4;++p)fft.outputs.push_back({p,PortDirection::Output,std::move(output[p])});
    auto result=numeric_test_builders::graph("fourier-numeric-ports",{1,120},
        {source,fft},{numeric_test_builders::edge(1,0,2,0)});
    if(!real && imaginaryConnected)result.edges.push_back(numeric_test_builders::edge(1,1,2,1));return result;
}
struct Observed{
    std::array<std::array<float,N>,4> values{};std::array<std::size_t,4> counts{},calls{};std::array<FrameStamp,4> stamps{};
    static void output(void* user,const NumericPortFrameView& event){
        if(event.identity.node!=2)return;auto& self=*static_cast<Observed*>(user);const auto p=event.identity.port;check(p<4,"output ID");
        ++self.calls[p];self.stamps[p]=event.frame.stamp;
        if(event.status!=Status::ok){self.counts[p]=0;return;}
        const auto& value=std::get<NumericView>(event.frame.payload);self.counts[p]=value.actual[0];
        check(value.contract->coordinates[0].mode==CoordinateMode::None,"FFT does not invent a frequency axis");
        for(std::size_t i=0;i<value.actual[0];++i)check(load_element(value,{0,i,0},self.values[p][i])==Status::ok,"read output");
    }
};
struct Running{
    NumericProcessingGraph g;std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;Observed seen;std::array<std::uint64_t,2> sequence{};
    Running(bool real,bool im=true):g(graph(real,im)){check(PreparedGraph::prepare_numeric(g,profile(),plan,state)==Status::ok,"prepare");}
    void feed(PortId lane,const std::array<float,N>& samples,std::int64_t begin=0,bool available=true,bool discontinuity=false,std::uint64_t producerEpoch=7,std::uint64_t generation=1){
        const auto data=g.nodes[0].outputs[lane].contract.view();const auto& c=std::get<NumericContract>(data);
        const PlaneLayout plane{0,{sizeof(float),0}};FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence[lane];stamp.producer_epoch=producerEpoch;stamp.generation=generation;
        stamp.timestamp_ticks=begin;stamp.sample_begin=begin;stamp.sample_count=N;stamp.has_sample_interval=true;stamp.available=available;
        stamp.discontinuity=discontinuity;
        std::span<const std::byte> bytes=std::as_bytes(std::span(samples));if(!available)bytes={};
        auto frame=numeric_test_builders::frame(stamp,c,bytes,std::span(&plane,1),{N,1});
        check(plan->process_numeric_ports(1,lane,frame,*state,{},NumericRuntimeCallbacks{Observed::output,nullptr,nullptr,&seen})==Status::ok,"feed");
    }
};
void complex_case(){
    Running run(false);std::array<float,N> re{},im{};
    for(std::size_t t=0;t<N;++t){re[t]=float(std::cos(6.283185307179586*3*t/N));im[t]=float(.2+std::sin(6.283185307179586*3*t/N)+.1*(t%2?-1:1));}
    run.feed(0,re);check(run.seen.calls[0]==0,"wait for imaginary plane");
    // Borrowed memory may change as soon as the synchronous ingress returns.
    const auto original=re;re.fill(0);run.feed(1,im);
    check(run.seen.counts[0]==N-1 && run.seen.counts[1]==N-1,"full complex spectrum excluding separate DC");
    check(std::abs(run.seen.values[0][2]-N)<.002,"positive complex tone");
    check(std::abs(run.seen.values[0][N-4])<.002,"negative spectrum independent");
    check(std::abs(run.seen.values[3][0]-.2*N)<.002,"imaginary DC scalar");
    check(std::abs(run.seen.values[1][N/2-1]-.1*N)<.002,"imaginary Nyquist not zeroed");
    check(run.seen.counts[2]==1 && run.seen.counts[3]==1,"separate scalar DC outputs");
    run.feed(0,original,N);run.feed(1,im,2*N);check(run.seen.calls[0]==1,"mismatched intervals never paired");
    run.feed(0,original,2*N);check(run.seen.calls[0]==2,"matching interval recovers");
    run.feed(1,im,3*N,true,true);run.feed(0,original,3*N,true,true);
    check(run.seen.calls[0]==3,"discontinuity does not lose a matching imaginary-first pair");
    check(run.seen.stamps[0].discontinuity,"paired boundary propagated");
    Running epochs(false);epochs.feed(0,original,0,true,false,8);epochs.feed(1,im,0,true,false,7);
    check(epochs.seen.calls[0]==0,"different producer epochs never paired");
    Running generations(false);generations.feed(0,original,0,true,false,7,2);generations.feed(1,im,0,true,false,7,1);
    check(generations.seen.calls[0]==0,"different generations never paired");
}
void incompatible_planes(){
    for(const auto* kind:{"Fourier","AmplitudePhase"})for(unsigned mismatch=0;mismatch<3;++mismatch){
        auto node=graph(false).nodes[1];
        if(std::string_view(kind)=="AmplitudePhase"){
            node.kind=kind;auto input=std::get<NumericContract>(node.outputs[0].contract.view());
            node.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t(N)}},{"positive_half",false}}};
            node.inputs={port(PortDirection::Input,0,input),port(PortDirection::Input,1,input)};
            std::array<OwnedProcessingContract,2> output;check(amplitude_phase_output_contracts(node,input,output)==Status::ok,"derive paired projection");
            node.outputs={{0,PortDirection::Output,output[0]},{1,PortDirection::Output,output[1]}};
        }
        auto second=std::get<NumericContract>(node.inputs[1].contract.view());
        if(mismatch==0)second.clock.domain="different-clock";
        else if(mismatch==1){--second.shape.extents[0].minimum;--second.shape.extents[0].maximum;}
        else second.coordinates[0]={CoordinateMode::Regular,NumericUnit::Values,{0,1},{2,1}};
        node.inputs[1].contract=port(PortDirection::Input,1,second).contract;
        ResolvedProcessingNode resolved;check(!resolve_processing_node(node,profile().admission,resolved),"paired operation rejects unequal clocks, lengths or coordinates");
    }
}
void real_case(){
    std::array<float,N> samples{};for(std::size_t i=0;i<N;++i)samples[i]=float(.25+.5*(i%2?-1:1));
    Running run(true);run.feed(0,samples);
    check(run.seen.counts[0]==N/2 && run.seen.counts[1]==N/2,"real nonzero spectrum has N/2 values");
    check(std::abs(run.seen.values[0][N/2-1]-.5*N)<.002,"final Nyquist point preserved");
    check(std::abs(run.seen.values[2][0]-.25*N)<.002 && run.seen.values[3][0]==0,"real DC split correctly");
    Running noImaginary(false,false);noImaginary.feed(0,samples);
    check(noImaginary.seen.calls[0]==1,"disconnected imaginary input gives explicit typed zero");
    for(std::size_t i=0;i<N/2;++i)check(std::abs(noImaginary.seen.values[0][i]-run.seen.values[0][i])<.002,"complex zero-imaginary agrees with Real Fourier");
}
void no_sample_rate_required(){
    auto g=graph(true);auto input=std::get<NumericContract>(g.nodes[1].inputs[0].contract.view());input.clock={};
    std::array<OwnedProcessingContract,4> output;check(fourier_output_contracts(g.nodes[1],input,output)==Status::ok,"Fourier derivation independent of Fs");
}
void polar_case(){
    auto g=graph(false);
    auto input=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    ConfiguredProcessingNode polar;polar.id=2;polar.kind="ComplexPolar";polar.parameters=TransformProcessingParameters{};
    polar.inputs={port(PortDirection::Input,0,input),port(PortDirection::Input,1,input)};
    std::array<OwnedProcessingContract,2> output;check(complex_polar_output_contracts(input,output)==Status::ok,"derive polar outputs");
    for(PortId p=0;p<2;++p)polar.outputs.push_back({p,PortDirection::Output,std::move(output[p])});
    g.nodes[1]=std::move(polar);
    Running run(false);run.g=std::move(g);
    check(PreparedGraph::prepare_numeric(run.g,profile(),run.plan,run.state)==Status::ok,"prepare polar");
    std::array<float,N> re{},im{};re.fill(3);im.fill(4);
    run.feed(0,re);check(run.seen.calls[0]==0,"polar waits for both planes");run.feed(1,im);
    check(run.seen.counts[0]==N && run.seen.counts[1]==N,"polar vector geometry");
    for(std::size_t i=0;i<N;++i){check(run.seen.values[0][i]==5,"3+4i magnitude is 5");check(std::abs(run.seen.values[1][i]-std::atan2(4.f,3.f))<1e-6,"phase is atan2");}
    for(const auto* kind:{"ComplexPolar","AmplitudePhase"})for(const PortId missing:{PortId{0},PortId{1}}){
        auto incomplete=run.g;
        if(std::string_view(kind)=="AmplitudePhase"){
            auto& node=incomplete.nodes[1];node.kind=kind;
            std::get<TransformProcessingParameters>(node.parameters).values.push_back({"positive_half",false});
            std::array<OwnedProcessingContract,2> derived;
            check(amplitude_phase_output_contracts(node,input,derived)==Status::ok,"derive incomplete normalized polar");
            node.outputs={{0,PortDirection::Output,derived[0]},{1,PortDirection::Output,derived[1]}};
        }
        std::erase_if(incomplete.edges,[&](const auto& edge){return edge.to==2 && edge.input_port==missing;});
        Running partial(false);partial.g=std::move(incomplete);
        check(PreparedGraph::prepare_numeric(partial.g,profile(),partial.plan,partial.state)==Status::ok,"prepare incomplete polar as unavailable");
        partial.feed(PortId(1-missing),missing?re:im);
        check(partial.seen.counts[0]==0 && partial.seen.counts[1]==0,"missing Re or Im never invents a zero plane or emits a magnitude");
    }
}
}
int main(){complex_case();incompatible_planes();real_case();no_sample_rate_required();polar_case();std::puts("PASS Fourier numeric ports: full complex input, matching owning planes, separate DC, real endpoint, polar projection, no Fs dependency");}
