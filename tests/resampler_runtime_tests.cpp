#include <dataflow_runtime/runtime.h>
#include <dsp_kernels/prepared_resampler.h>
#include <speex_backend.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
using namespace signal_contracts;using namespace domain;using namespace dataflow_runtime;
constexpr std::array<std::uint32_t,1> mono{0};
constexpr FrameLimits limits{1,32768,262144,16384,256,8192};
NumericRuntimeProfile profile(){auto p=NumericRuntimeProfile{{16,32,8,16,1048576,4*1048576,limits},{{4*1048576,4*1048576,4*1048576,131072},0,32*1048576},{32768,1,262144,262144,1048576},{true,true,true}};
    p.music_enabled=true;p.experimental_resampling_enabled=true;p.resampler_backend=&desktop_resampling::speex_backend_factory();return p;}
ProcessingPort port(PortDirection direction,const NumericContract& c){ProcessingPort p;p.id=0;p.direction=direction;assert(own_processing_contract(c,limits,65536,p.contract)==Status::ok);return p;}
struct Observation {
    std::array<float,4096> values{};std::array<std::int64_t,4096> ticks{};std::size_t count=0;bool nonlinear=false;
    static void observe(void* user,const NumericPortFrameView& event){if(event.identity.node!=2)return;
        assert(event.status==Status::ok);auto& self=*static_cast<Observation*>(user);const auto& view=std::get<NumericView>(event.frame.payload);
        assert(self.count+view.actual[0]<=self.values.size());self.nonlinear=self.nonlinear||event.frame.stamp.frequency_dependent_latency;
        for(std::size_t i=0;i<view.actual[0];++i){assert(load_element(view,{0,i,0},self.values[self.count])==Status::ok);
            self.ticks[self.count++]=event.frame.stamp.timestamp_ticks+std::int64_t(i);}
    }
};
void origin(const char* mode,unsigned in,unsigned out,std::int64_t start,bool replace){
    NumericContract input;input.channel_order=mono;input.semantic=Semantic::PCM;input.quantity=Quantity::Amplitude;input.unit=NumericUnit::NormalizedAmplitude;
    input.representation.normalization=Normalization::FloatUnit;input.representation.alignment=alignof(float);
    input.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{1,127},ExtentBounds{}}};input.clock={"resampler-clock",{1,in},in};
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={port(PortDirection::Output,input)};
    ConfiguredProcessingNode resampler;resampler.id=2;resampler.kind="PcmResampler";resampler.inputs={port(PortDirection::Input,input)};
    resampler.parameters=TransformProcessingParameters{{{"mode",std::string(mode)},{"input_rate",NumericLiteral{std::uint64_t(in)}},{"output_rate",NumericLiteral{std::uint64_t(out)}}}};
    auto output=input;output.clock.sample_rate=out;output.clock.tick_seconds={1,out};
    output.shape.extents[0]={1,dsp_kernels::PcmResampler::maximum_output(127,in,out)};
    resampler.outputs={port(PortDirection::Output,output)};
    NumericProcessingGraph graph;graph.graph_id="resampler-origin-regression";graph.nodes={source,resampler};graph.edges={{1,2}};
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    assert(PreparedGraph::prepare_numeric(graph,profile(),plan,context)==Status::ok);Observation observed;
    std::array<float,127> samples{};std::size_t offset=0;std::uint64_t sequence=0;
    while(offset<1501){const auto count=std::min<std::size_t>(127,1501-offset);
        for(std::size_t i=0;i<count;++i)samples[i]=float(double(start+std::int64_t(offset+i))/4096.);
        FrameStamp stamp;stamp.sequence=++sequence;stamp.generation=1;stamp.producer_epoch=5;
        stamp.timestamp_ticks=stamp.sample_begin=start+std::int64_t(offset);stamp.sample_count=count;stamp.has_sample_interval=true;
        const PlaneLayout layout{0,{sizeof(float),0}};NumericView view{&input,std::as_bytes(std::span(samples)),std::span(&layout,1),{count,1},{},{}};
        const auto status=plan->process_numeric_ports(1,{stamp,view},*context,{}, {Observation::observe,nullptr,nullptr,&observed});
        if(status!=Status::ok)std::fprintf(stderr,"origin mode=%s %u->%u start=%lld offset=%zu count=%zu status=%u\n",mode,in,out,static_cast<long long>(start),offset,count,unsigned(status));
        assert(status==Status::ok);
        offset+=count;
        if(replace && sequence==4){std::shared_ptr<const PreparedGraph> next;std::unique_ptr<RuntimeContext> state;
            assert(PreparedGraph::prepare_numeric(graph,profile(),next,state,plan.get(),context.get())==Status::ok);plan=std::move(next);context=std::move(state);}
    }
    const auto first=std::int64_t(std::ceil(double(start)*out/in));assert(observed.count>100);
    for(std::size_t i=0;i<observed.count;++i)assert(observed.ticks[i]==first+std::int64_t(i));
    if(std::string_view(mode)=="linear")for(std::size_t i=0;i<observed.count;++i){
        const double expected=double(observed.ticks[i])*in/out/4096.;assert(std::abs(observed.values[i]-expected)<2e-6);}
    if(std::string_view(mode)=="iir")assert(observed.nonlinear);else assert(!observed.nonlinear);
    // A discontinuity must re-anchor DSP phase on the new global grid rather
    // than retaining the previous stream's fractional origin.
    observed={};const auto restart=-start-2003;
    for(std::size_t packet=0;packet<3;++packet){
        const auto tick=restart+std::int64_t(packet*samples.size());
        for(std::size_t i=0;i<samples.size();++i)samples[i]=float(double(tick+std::int64_t(i))/4096.);
        FrameStamp stamp;stamp.sequence=++sequence;stamp.generation=1;stamp.producer_epoch=5;stamp.discontinuity=packet==0;
        stamp.timestamp_ticks=stamp.sample_begin=tick;stamp.sample_count=samples.size();stamp.has_sample_interval=true;
        const PlaneLayout layout{0,{sizeof(float),0}};NumericView view{&input,std::as_bytes(std::span(samples)),std::span(&layout,1),{samples.size(),1},{},{}};
        assert(plan->process_numeric_ports(1,{stamp,view},*context,{}, {Observation::observe,nullptr,nullptr,&observed})==Status::ok);
    }
    const auto restarted_first=std::int64_t(std::ceil(double(restart)*out/in));assert(observed.count>50);
    for(std::size_t i=0;i<observed.count;++i){assert(observed.ticks[i]==restarted_first+std::int64_t(i));
        if(std::string_view(mode)=="linear")assert(std::abs(observed.values[i]-double(observed.ticks[i])*in/out/4096.)<2e-6);}
}
void nonlinear_alignment_rejected(){
    NumericContract c;c.channel_order=mono;c.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{1,127},ExtentBounds{}}};
    c.clock={"aligned",{1,48000},48000};c.representation.alignment=alignof(float);
    ConfiguredProcessingNode a;a.id=1;a.kind="NetworkInput";a.outputs={port(PortDirection::Output,c)};
    ConfiguredProcessingNode mixer;mixer.id=3;mixer.kind="Mixer";
    mixer.inputs={port(PortDirection::Input,c),port(PortDirection::Input,c)};mixer.inputs[1].id=1;
    mixer.outputs={port(PortDirection::Output,c)};
    mixer.parameters=MixerProcessingParameters{MixerMode::WeightedSum,{{0,.5},{1,.5}},100,MixerConsumption::SourceAligned};
    NumericProcessingGraph graph;graph.graph_id="nonlinear-alignment";graph.nodes={a,mixer};
    graph.edges={{1,3,0,0},{1,3,0,1}};
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    const auto prepared=PreparedGraph::prepare_numeric(graph,profile(),plan,context);
    if(prepared!=Status::ok){
        std::fprintf(stderr,"nonlinear fixture prepare=%u\n",unsigned(prepared));
    }
    assert(prepared==Status::ok);
    std::array<float,127> samples{};const PlaneLayout layout{0,{4,0}};
    FrameStamp stamp;stamp.sequence=1;stamp.producer_epoch=1;stamp.generation=1;stamp.has_sample_interval=true;
    stamp.sample_count=127;stamp.frequency_dependent_latency=true;
    NumericView view{&c,std::as_bytes(std::span(samples)),std::span(&layout,1),{127,1},{},{}};
    assert(plan->process_numeric_ports(1,{stamp,view},*context,{})==Status::incompatible_contract);
}
}
int main(){
    nonlinear_alignment_rejected();
    for(const auto mode:{"existing_fir","compact_fir","speex","iir","linear"})
        for(const auto in:{48000u,44100u})for(const auto out:{32768u,32000u,36000u})for(const auto start:{1ll,137ll,-137ll})
            origin(mode,in,out,start,true);
    std::puts("resampler_runtime_tests PASS: global grid, positive/negative origin, every mode, compatible Apply, nonlinear delay provenance");
}
