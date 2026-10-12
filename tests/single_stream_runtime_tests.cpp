#include <dataflow_runtime/runtime.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
using namespace signal_contracts;
using namespace domain;
using namespace dataflow_runtime;
std::size_t allocations=0;
void require(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
constexpr FrameLimits frame_limits{8,32768,262144,16384,256,8192};
constexpr std::array<std::uint32_t,1> mono{0};
constexpr double tau=6.283185307179586476925286766559;
NumericRuntimeProfile profile(){return {{32,64,128,16,256,65536,frame_limits},{{4194304,4194304,4194304,131072},0,16777216},{32768,8,262144,262144,262144},{true,true,true}};}
NumericContract vector_contract(std::size_t maximum=256,bool spectral=false,std::uint32_t rate=48000){
 NumericContract c;c.dtype=DType::Float32;c.semantic=Semantic::Numeric;c.channel_order=mono;
 c.shape={Rank::Vector,{spectral?Axis::Bin:Axis::Sample,Axis::None},{ExtentBounds{spectral?maximum:0,maximum},ExtentBounds{}}};
 c.clock={"local",{1,rate},rate};return c;
}
ProcessingPort port(PortDirection direction,PortId id,const NumericContract& c){ProcessingPort p;p.id=id;p.direction=direction;require(own_processing_contract(c,frame_limits,65536,p.contract)==Status::ok,"own fixture contract");return p;}
ConfiguredProcessingNode source(NodeId id,const NumericContract& c){ConfiguredProcessingNode n;n.id=id;n.kind="NetworkInput";n.outputs.push_back(port(PortDirection::Output,0,c));return n;}
ConfiguredProcessingNode clock_node(std::uint32_t rate=48000){ConfiguredProcessingNode n;n.id=10;n.kind="SampleClock";n.parameters=SampleClockProcessingParameters{rate,"local"};return n;}
NumericProcessingGraph generator_graph(){auto c=vector_contract();ConfiguredProcessingNode n;n.id=1;n.kind="SignalGenerator";n.outputs.push_back(port(PortDirection::Output,0,c));n.parameters=SignalGeneratorProcessingParameters{{{77,true,1000,2,0,0,0}}};NumericProcessingGraph g;g.graph_id="single-stream-regression";g.nodes={clock_node(),n};g.configuration_bindings={{10,1}};return g;}
NumericProcessingGraph mixer_graph(bool spectral=false,MixerMode mode=MixerMode::WeightedSum,std::uint32_t rate=48000){
 auto c=vector_contract(spectral?4:256,spectral,rate);ConfiguredProcessingNode m;m.id=3;m.kind="Mixer";
 // Deliberately sparse and unsorted identities: PortId must not be a vector index.
 m.inputs={port(PortDirection::Input,9,c),port(PortDirection::Input,5,c)};m.outputs={port(PortDirection::Output,0,c)};
 m.parameters=MixerProcessingParameters{mode,{{9,.25},{5,.75}},1};
 NumericProcessingGraph g;g.graph_id="single-stream-regression";g.nodes={clock_node(rate),source(1,c),source(2,c),m};g.edges={{1,3,0,9},{2,3,0,5}};g.configuration_bindings={{10,3}};return g;
}
ConfiguredProcessingNode& node(NumericProcessingGraph& g,NodeId id){auto at=std::find_if(g.nodes.begin(),g.nodes.end(),[&](const auto& n){return n.id==id;});require(at!=g.nodes.end(),"fixture node exists");return *at;}
struct Running {
 std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
 explicit Running(const NumericProcessingGraph& g,NumericRuntimeProfile p=profile()){require(PreparedGraph::prepare_numeric(g,p,plan,state)==Status::ok,"prepare independent graph");}
 void replace(const NumericProcessingGraph& g){std::shared_ptr<const PreparedGraph> next;std::unique_ptr<RuntimeContext> context;require(PreparedGraph::prepare_numeric(g,profile(),next,context,plan.get(),state.get())==Status::ok,"prepare compatible replacement");plan=std::move(next);state=std::move(context);}
};
struct Observation {
 NodeId watched=3;std::size_t calls=0,count=0;std::array<float,512> values{};FrameStamp stamp{};
 static void output(void* user,const NumericPortFrameView& f){auto& o=*static_cast<Observation*>(user);if(f.identity.node!=o.watched)return;require(f.status==Status::ok,"observed output available");const auto& v=std::get<NumericView>(f.frame.payload);require(v.actual[0]<=o.values.size(),"bounded callback observation");++o.calls;o.count=v.actual[0];o.stamp=f.frame.stamp;o.stamp.stream_id={};for(std::size_t i=0;i<o.count;++i)require(load_element(v,{0,i,0},o.values[i])==Status::ok,"load observed element");}
 NumericRuntimeCallbacks callbacks(){return {output,nullptr,nullptr,this};}
};
FrameStamp tick(std::int64_t begin,std::uint64_t count,std::uint64_t sequence,bool discontinuity=false){FrameStamp s;s.producer_epoch=17;s.generation=1;s.sequence=sequence;s.source_sequence=sequence;s.has_sample_interval=true;s.sample_begin=begin;s.sample_count=count;s.timestamp_ticks=begin+std::int64_t(count)-1;s.discontinuity=discontinuity;return s;}
void clock(Running& r,FrameStamp s,Observation& o){const auto before=allocations;require(r.plan->process_clock(10,s,*r.state,{},o.callbacks())==Status::ok,"process clock");require(allocations==before,"clock hot path does not allocate");}
void feed(Running& r,const NumericProcessingGraph& g,NodeId id,std::span<const float> data,std::uint64_t sequence,Ratio delay={0,1}){
 const auto& n=*std::find_if(g.nodes.begin(),g.nodes.end(),[&](const auto& v){return v.id==id;});auto c=std::get<NumericContract>(n.outputs[0].contract.view());const PlaneLayout plane{0,{sizeof(float),0}};auto s=tick(std::int64_t((sequence-1)*256),256,sequence);s.latency_seconds=delay;
 std::array<float,512> storage{};require(data.size()<=storage.size(),"bounded fixture ingress");std::copy(data.begin(),data.end(),storage.begin());
 DataFrameView f{s,NumericView{&c,std::as_bytes(std::span(storage)),std::span(&plane,1),{data.size(),1},{},{}}};const auto before=allocations;
 const auto status=r.plan->process_numeric_ports(id,f,*r.state,{});if(status!=Status::ok)std::fprintf(stderr,"ingress node=%d status=%d actual=%zu sequence=%llu\n",int(id),int(status),data.size(),(unsigned long long)sequence);require(status==Status::ok,"ingress accepted");require(allocations==before,"ingress hot path does not allocate");
}
void near(double actual,double expected,const char* message,double tolerance=2e-5){require(std::abs(actual-expected)<=tolerance,message);}
void constant(const Observation& o,float expected){require(o.count>0,"nonempty output");for(std::size_t i=0;i<o.count;++i)near(o.values[i],expected,"all output elements follow arithmetic oracle");}

void generator_oracles(){
 auto g=generator_graph();Running r(g);Observation o;o.watched=1;
 clock(r,tick(0,256,1),o);require(o.count==256,"generator emits one mono output");for(std::size_t i=0;i<o.count;++i)near(o.values[i],2*std::sin(tau*1000*i/48000.),"single stationary tone; no normalization or sweep");
 clock(r,tick(256,256,2),o);for(std::size_t i=0;i<o.count;++i)near(o.values[i],2*std::sin(tau*1000*(256+i)/48000.),"phase continuity across callbacks");
 clock(r,tick(2048,256,3),o);require(o.stamp.discontinuity,"shared clock gap is explicit");for(std::size_t i=0;i<o.count;++i)near(o.values[i],2*std::sin(tau*1000*(2048+i)/48000.),"gap phase uses actual sample position");
 auto& parameters=std::get<SignalGeneratorProcessingParameters>(node(g,1).parameters);
 parameters.oscillators.push_back({3,true,700,.3,.2,0,0});r.replace(g);clock(r,tick(2304,256,4),o);
 for(std::size_t i=0;i<o.count;++i){const auto t=double(2304+i)/48000.;near(o.values[i],2*std::sin(tau*1000*t)+.3*std::sin(tau*700*t+.2),"new oscillator ID shares timeline");}
 // Reorder stable IDs and explicitly edit one phase; unchanged oscillator must continue.
 std::reverse(parameters.oscillators.begin(),parameters.oscillators.end());parameters.oscillators[1].phase_radians=.4;r.replace(g);clock(r,tick(2560,256,5),o);
 for(std::size_t i=0;i<o.count;++i)near(o.values[i],2*std::sin(.4+tau*1000*i/48000.)+.3*std::sin(tau*700*(2560+i)/48000.+.2),"ID reorder and explicit phase edit");
 // A generator added to a running shared clock must not restart at sample zero.
 auto second=node(g,1);second.id=2;second.parameters=SignalGeneratorProcessingParameters{{{99,true,300,.1,.7,0,0}}};g.nodes.push_back(second);g.configuration_bindings.push_back({10,2});r.replace(g);o.watched=2;clock(r,tick(2816,256,6),o);
 near(o.values[0],.1*std::sin(tau*300*2816/48000.+.7),"new generator joins shared sample position");
 auto envelope_graph=generator_graph();auto& envelope=std::get<SignalGeneratorProcessingParameters>(node(envelope_graph,1).parameters).oscillators.front();envelope.amplitude=1;envelope.frequency_hz=500;envelope.phase_radians=tau/4;envelope.attack_seconds=.001;envelope.release_seconds=.001;
 Running envelope_run(envelope_graph);Observation ramp;ramp.watched=1;clock(envelope_run,tick(0,48,1),ramp);
 for(std::size_t i=0;i<48;++i)near(ramp.values[i],double(i+1)/48*std::sin(tau/4+tau*500*i/48000.),"individual attack envelope");
 envelope.enabled=false;envelope_run.replace(envelope_graph);clock(envelope_run,tick(48,48,2),ramp);
 for(std::size_t i=0;i<48;++i)near(ramp.values[i],double(47-i)/48*std::sin(tau/4+tau*500*(48+i)/48000.),"individual disable release envelope");
 std::puts("single-stream generator numerical/timeline/ID/phase oracles PASS");
}

void mixer_modes(){
 const std::array<float,4> a{4,4,4,4},b{-2,-2,-2,-2};
 for(auto mode:{MixerMode::WeightedSum,MixerMode::WeightedMean,MixerMode::WeightedRMS,MixerMode::MaxAbsolute}){
  auto g=mixer_graph(false,mode);Running r(g);Observation o;feed(r,g,1,a,1);feed(r,g,2,b,1);clock(r,tick(0,4,1),o);
  const float expected=mode==MixerMode::WeightedRMS?float(std::sqrt(7.)):mode==MixerMode::MaxAbsolute?-1.5f:-.5f;constant(o,expected);
  clock(r,tick(4,4,2),o);constant(o,0);require(r.plan->mixer_input_stats(3,9,*r.state)->underrun_samples==4,"underrun counted per sparse port");
 }
 // Weights need not sum to one: sum and mean are distinct operations.
 for(auto mode:{MixerMode::WeightedSum,MixerMode::WeightedMean}){auto g=mixer_graph(false,mode);auto& p=std::get<MixerProcessingParameters>(node(g,3).parameters);p.weights={{9,2},{5,1}};Running r(g);Observation o;feed(r,g,1,a,1);feed(r,g,2,b,1);clock(r,tick(0,4,1),o);constant(o,mode==MixerMode::WeightedSum?6.f:2.f);}
 std::puts("explicit Mixer arithmetic, sparse ports, zero underrun PASS");
}

void fifo_rebind_and_latency(){
 auto g=mixer_graph();Running r(g);Observation o;
 const std::array<float,8> a{4,4,4,4,8,8,8,8},b{-2,-2,-2,-2,-6,-6,-6,-6};
 const Ratio close{9007199254740993LL,9007199254740992ULL};
 feed(r,g,1,a,1,{1,1});feed(r,g,2,b,1,close);clock(r,tick(0,4,1),o);
 require(o.stamp.latency_seconds==close,"exact latency max distinguishes ratios below double precision");require(o.stamp.latency_samples==48000,"latency diagnostic uses output Fs");constant(o,-.5f);
 // Rebind only input9; input5's pending suffix remains valid.
 auto c=std::get<NumericContract>(node(g,1).outputs[0].contract.view());g.nodes.push_back(source(4,c));g.edges[0].from=4;r.replace(g);clock(r,tick(4,4,2),o);constant(o,-4.5f);require(o.stamp.latency_seconds==close,"unaffected FIFO provenance retained on Apply");
 clock(r,tick(8,4,3),o);constant(o,0);require(o.stamp.latency_seconds==Ratio{0,1}&&o.stamp.latency_samples==0,"empty FIFO does not reuse stale latency");
 // Overflow retains the newest bounded suffix and matching latency provenance.
 std::array<float,256> full{};full.fill(3);feed(r,g,4,full,1,{7,10});full.fill(5);feed(r,g,4,full,2,{9,10});
 require(r.plan->mixer_input_stats(3,9,*r.state)->dropped_samples==256,"bounded PCM FIFO drops oldest samples");clock(r,tick(12,4,4),o);constant(o,1.25f);require(o.stamp.latency_seconds==Ratio{9,10},"drop retains latency of actual surviving samples");
 std::puts("PCM FIFO partial consume/rebind/drop/latency/no-stale provenance PASS");
}

void spectral_cadence(){
 const std::array<float,4> a{1,2,3,4},b{4,3,2,1};
 for(const auto rate:{60,90,100,120}){auto g=mixer_graph(true);g.execution_interval={1,std::uint64_t(rate)};Running r(g);Observation o;std::uint64_t sequence=0;
  for(std::int64_t begin=0;begin<48000;begin+=256){const auto count=std::uint64_t(std::min<std::int64_t>(256,48000-begin));++sequence;feed(r,g,1,a,sequence,{1,1});feed(r,g,2,b,sequence,{9007199254740993LL,9007199254740992ULL});clock(r,tick(begin,count,sequence),o);
   require(o.calls==std::size_t((begin+std::int64_t(count))*rate/48000),"spectral exact deadlines independent of callback block frequency");}
  require(o.calls==std::size_t(rate)&&o.stamp.timestamp_ticks==47999,"one second exact spectral cadence");require(o.stamp.latency_seconds==Ratio{9007199254740993LL,9007199254740992ULL}&&o.stamp.latency_samples==48000,"spectral exact latency propagation");
  for(std::size_t i=0;i<4;++i)near(o.values[i],.25*a[i]+.75*b[i],"whole spectral frames preserved");
 }
 std::puts("spectral 60/90/100/120 rational cadence and exact latency PASS");
}

void rejection_preserves_working_graph(){
 auto g=generator_graph();GraphExecutor e(profile());application::PreparedExecution pending;auto p=profile();require(e.prepare_numeric(g,{p.admission,p.resources},pending)==Status::ok&&e.apply(std::move(pending))==Status::ok,"executor working graph admitted");
 auto bad=mixer_graph(true);auto c=std::get<NumericContract>(node(bad,3).outputs[0].contract.view());c.shape.extents[0]={2,2};node(bad,3).outputs[0]=port(PortDirection::Output,0,c);
 require(e.prepare_numeric(bad,{p.admission,p.resources},pending)!=Status::ok,"inconsistent spectral geometry rejected");
 auto variable=mixer_graph(true);c=std::get<NumericContract>(node(variable,1).outputs[0].contract.view());c.shape.extents[0]={1,4};node(variable,1).outputs[0]=port(PortDirection::Output,0,c);node(variable,3).inputs[0]=port(PortDirection::Input,9,c);
 require(e.prepare_numeric(variable,{p.admission,p.resources},pending)!=Status::ok,"variable spectral frame geometry rejected");
 auto restrictive=p.resources;restrictive.max_peak_bytes=1;require(e.prepare_numeric(g,{p.admission,restrictive},pending)!=Status::ok,"insufficient resource admission rejected");
 Observation o;o.watched=1;const auto before=allocations;require(e.process_clock(10,tick(0,256,1),{},o.callbacks())==Status::ok&&allocations==before,"rejected candidate leaves working graph allocation-free");require(o.calls==1,"working graph remains usable after rejected candidates");
 std::puts("geometry/resource rejection atomicity and working-graph continuity PASS");
}

void node_stats_oracles(){
 struct FakeTime {
  std::uint64_t elapsed=0,delivery_delay=0;
  static std::uint64_t read(void* p) noexcept {auto& t=*static_cast<FakeTime*>(p);const auto current=t.elapsed;t.elapsed+=3000000;return current;}
  static void delivery(void* p,const NumericPortFrameView&){auto& t=*static_cast<FakeTime*>(p);t.elapsed+=t.delivery_delay;}
 };
 auto graph=[](std::uint64_t rate,bool deep){
  auto c=vector_contract(4,true);NumericProcessingGraph g;g.graph_id="stats";g.execution_interval={1,rate};g.nodes.push_back(source(1,c));
  ConfiguredProcessingNode gain;gain.id=2;gain.kind="SpectrumGain";gain.inputs={port(PortDirection::Input,0,c)};gain.outputs={port(PortDirection::Output,0,c)};
  gain.parameters=TransformProcessingParameters{{{"gain",NumericLiteral{2.0}}}};g.nodes.push_back(gain);g.edges.push_back({1,2,0,0});
  if(deep){gain.id=3;gain.parameters=TransformProcessingParameters{{{"gain",NumericLiteral{.5}}}};g.nodes.push_back(gain);g.edges.push_back({2,3,0,0});}
  ConfiguredProcessingNode sink;sink.id=4;sink.kind="FFTView";sink.inputs={port(PortDirection::Input,0,c)};g.nodes.push_back(sink);g.edges.push_back({deep?3:2,4,0,0});return g;
 };
 // A monotonic clock with a known resolution supplies synthetic work time.
 // Delivery consumes an additional20ms, independent of kernel topology/work.
 // Comparing identical runs with/without that cost catches callback charging;
 // adding a descendant catches inclusive time charged again to ancestors.
 for(const auto rate:{90u,120u})for(const bool deep:{false,true}){
  std::array<NodeStats,4> reference{};
  for(const auto delivery_delay:{std::uint64_t{0},std::uint64_t{20000000}}){
   auto g=graph(rate,deep);FakeTime time;time.delivery_delay=delivery_delay;auto p=profile();p.monotonic_nanoseconds=FakeTime::read;p.measurement_clock=&time;Running r(g,p);
   const auto contract=std::get<NumericContract>(node(g,1).outputs[0].contract.view());std::array<float,4> samples{1,2,3,4};const PlaneLayout plane{0,{sizeof(float),0}};
   auto process=[&](std::uint64_t sequence){const DataFrameView frame{tick(std::int64_t((sequence-1)*4),4,sequence),NumericView{&contract,std::as_bytes(std::span(samples)),std::span(&plane,1),{4,1}}};
    const auto before=allocations;const auto status=r.plan->process_numeric_ports(1,frame,*r.state,{}, {FakeTime::delivery,FakeTime::delivery,nullptr,&time});require(allocations==before,"measured ingress and stats do not allocate");return status;};
   require(process(1)==Status::ok && process(2)==Status::ok,"measured numerical chain processes frames");
   for(const auto id:{1,2,3,4}){if(id==3&&!deep)continue;const auto stats=r.plan->node_stats(id,*r.state);require(bool(stats)&&stats->node==id,"stats retain stable node identity");
    require(stats->calls==2 && stats->produced==(id==4?0u:2u) && stats->errors==0,"per-node calls/produced/errors reflect actual execution");
    require(stats->self_nanoseconds>0 && stats->self_nanoseconds==2*stats->max_self_nanoseconds,"repeatable work has exact cumulative/max selftime");
    std::uint64_t observations=0;for(auto count:stats->self_histogram)observations+=count;require(observations==stats->calls,"histogram records every measured execution");
    const bool exceeds=stats->max_self_nanoseconds*rate>1000000000ULL;
    require(stats->deadline_misses==(exceeds?2u:0u),"deadline misses use configured rational analysis interval");
    if(!delivery_delay)reference[id-1]=*stats;else{const auto& baseline=reference[id-1];require(stats->self_nanoseconds==baseline.self_nanoseconds && stats->max_self_nanoseconds==baseline.max_self_nanoseconds && stats->deadline_misses==baseline.deadline_misses && stats->self_histogram==baseline.self_histogram,"delivery latency cannot alter reported kernel selftime");}
   }
   const auto source_stats=*r.plan->node_stats(1,*r.state),gain_stats=*r.plan->node_stats(2,*r.state);
   require(source_stats.max_self_nanoseconds==gain_stats.max_self_nanoseconds,"ancestor selftime excludes descendants");
   require(source_stats.deadline_misses==(rate==120?2u:0u),"same work crosses120Hz budget but fits90Hz budget");
   // Finite input whose multiplication overflows must count as a kernel error,
   // even though the executor continues delivery with an unavailable marker.
   samples.fill(3.4028234663852886e38f);require(process(3)==Status::overflow,"overflow is reported without stopping the graph");
   const auto failed=*r.plan->node_stats(2,*r.state);require(failed.calls==3&&failed.produced==3&&failed.errors==1,"failed transform updates exact error/call/marker counters");
   samples.fill(1);require(process(4)==Status::ok,"healthy data continues after a measured kernel error");
   const auto recovered=*r.plan->node_stats(2,*r.state);require(recovered.calls==4&&recovered.produced==4&&recovered.errors==1,"recovery retains error diagnostics without inventing new errors");
   require(!r.plan->node_stats(99,*r.state),"unknown node has no fabricated stats");
  }
 }
 std::puts("NodeStats fakeclock exclusive selftime/callback isolation/counters/histogram/rational deadlines PASS");
}

void resampler_latency(bool spectral){
 auto g=mixer_graph(spectral,MixerMode::WeightedSum,24000);auto input=vector_contract(256,false,48000),pcm=vector_contract(129,false,24000);pcm.shape.extents[0].minimum=1;
 node(g,1).outputs[0]=port(PortDirection::Output,0,input);
 ConfiguredProcessingNode resampler;resampler.id=20;resampler.kind="PcmResampler";resampler.parameters=TransformProcessingParameters{{{"output_rate",NumericLiteral{std::uint64_t(24000)}}}};resampler.inputs={port(PortDirection::Input,0,input)};resampler.outputs={port(PortDirection::Output,0,pcm)};g.nodes.push_back(resampler);g.edges[0].from=20;
 if(!spectral){node(g,2).outputs[0]=port(PortDirection::Output,0,pcm);node(g,3).inputs={port(PortDirection::Input,9,pcm),port(PortDirection::Input,5,pcm)};node(g,3).outputs={port(PortDirection::Output,0,pcm)};}
 else {
  auto window=pcm;window.shape.extents[0]={512,512};auto bins=vector_contract(256,true,24000);bins.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{24000,512}};
  ConfiguredProcessingNode w;w.id=21;w.kind="Window";w.parameters=TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t(512)}}}};w.inputs={port(PortDirection::Input,0,pcm)};w.outputs={port(PortDirection::Output,0,window)};
  ConfiguredProcessingNode fft;fft.id=22;fft.kind="StereoFFT";fft.parameters=TransformProcessingParameters{};fft.inputs={port(PortDirection::Input,0,window)};fft.outputs={port(PortDirection::Output,0,bins)};
  node(g,2).outputs={port(PortDirection::Output,0,bins)};node(g,3).inputs={port(PortDirection::Input,9,bins),port(PortDirection::Input,5,bins)};node(g,3).outputs={port(PortDirection::Output,0,bins)};
  g.nodes.push_back(w);g.nodes.push_back(fft);g.edges[0].from=22;g.edges.push_back({20,21,0,0});g.edges.push_back({21,22,0,0});
 }
 g.edges.push_back({1,20,0,0});Running r(g);Observation o;std::array<float,256> values{};values.fill(.25f);
 for(std::uint64_t i=0;i<8;++i){feed(r,g,1,values,i+1);clock(r,tick(std::int64_t(i*128),128,i+1),o);}
 require(o.calls>0,"resampled chain produces Mixer output");require(o.stamp.latency_seconds==Ratio{21,16000}&&o.stamp.latency_samples==31,"Resampler algorithmic latency survives PCM/FFT/Mixer in output Fs");
 std::puts(spectral?"Resampler -> Window -> FFT -> spectral Mixer latency PASS":"Resampler -> temporal Mixer latency PASS");
}
}
void* operator new(std::size_t n){++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
int main(){generator_oracles();mixer_modes();fifo_rebind_and_latency();spectral_cadence();rejection_preserves_working_graph();node_stats_oracles();resampler_latency(false);resampler_latency(true);std::puts("single-stream runtime regression package PASS");}
