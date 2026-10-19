#pragma once
#include <dsp_kernels/dsp.h>
#include <limits>
#include <bit>
namespace core_regression {
using namespace canonical_test;
inline OwnedProcessingContract own(const NumericContract& c){OwnedProcessingContract o;check(own_processing_contract(c,limits,65536,o)==Status::ok,"regression contract ownership");return o;}
inline NumericProcessingGraph short_window(std::size_t rate=48000,std::size_t size=1024,std::size_t block=4096){auto g=oracle_graph();geometry(g,rate,size,block);std::erase_if(g.nodes,[](const auto& n){return n.id>2;});g.configuration_bindings.clear();g.edges={{1,2,0,0}};return g;}
struct Collected {
 NodeId target=2;std::size_t count=0,width=0;Status last=Status::ok;bool available=true;
 std::array<std::int64_t,128> ticks{};std::array<bool,128> discontinuities{};
 std::vector<float> data=std::vector<float>(128*2048);
};
inline void collect(void* user,const NumericPortFrameView& f){auto& out=*static_cast<Collected*>(user);if(f.identity.node!=out.target)return;
 out.last=f.status;out.available=f.frame.stamp.available;if(f.status!=Status::ok)return;
 const auto& v=std::get<NumericView>(f.frame.payload);out.width=v.actual[0]*v.planes.size();check(out.width<=2048&&out.count<128,"bounded regression observations");
 out.ticks[out.count]=f.frame.stamp.timestamp_ticks;out.discontinuities[out.count]=f.frame.stamp.discontinuity;
 for(std::size_t p=0;p<v.planes.size();++p)for(std::size_t j=0;j<v.actual[0];++j)check(load_element(v,{p,j,0},out.data[out.count*2048+p*v.actual[0]+j])==Status::ok,"observe numeric output");++out.count;
}
struct Pcm {
 NumericContract contract;std::vector<float> data;std::array<PlaneLayout,1> planes;std::uint64_t offset=0,sequence=0;
 explicit Pcm(const NumericProcessingGraph& g):contract(std::get<NumericContract>(g.nodes.front().outputs[0].contract.view())),data(contract.shape.extents[0].maximum){planes={PlaneLayout{0,{4,0}}};}
 Status send(GraphExecutor& e,std::size_t n,Collected& c,float constant=std::numeric_limits<float>::quiet_NaN()){
  const auto maximum=std::size_t(contract.shape.extents[0].maximum);
  check(n<=maximum,"PCM fixture packet exceeds allocated contract capacity");
  for(std::size_t p=0;p<1;++p)for(std::size_t i=0;i<n;++i){double value=constant;
   if(std::isnan(constant)){value=0;for(unsigned tone=0;tone<7;++tone)value+=.025*std::sin(6.283185307179586*(offset+i)*(73+tone*137+p*31)/contract.clock.sample_rate);}
   data[p*maximum+i]=float(value);}
  FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=17;stamp.generation=4;stamp.timestamp_ticks=std::int64_t(offset);offset+=n;
  const auto before=allocations;const auto status=e.process_numeric_ports(1,{stamp,NumericView{&contract,std::as_bytes(std::span(data)),planes,{n,1},{},{}}},{},{collect,nullptr,nullptr,&c});
  check(allocations==before,"new regression hot allocation");return status;
 }
};
inline Collected partition(std::size_t partition_size,bool random=false){auto g=short_window(44101,512);g.execution_interval={1,37};GraphExecutor e(profile());application::PreparedExecution prepared;prepare(e,g,prepared);check(e.apply(std::move(prepared))==Status::ok&&e.running(),"Apply auto-runs partition");Pcm pcm(g);Collected c;std::uint32_t seed=123;
 while(pcm.offset<44101){seed=seed*1664525+1013904223;const auto size=random?1+seed%4096:partition_size;check(pcm.send(e,std::min<std::size_t>(size,44101-pcm.offset),c)==Status::ok,"partition processing");}
 check(c.count==37&&c.ticks[36]==44101,"rational cadence one-second oracle");return c;
}
inline void interval_partition_gap_bypass(){const auto expected=partition(4096);for(auto size:{std::size_t{1},std::size_t{256}}){const auto got=partition(size);check(got.count==expected.count&&got.width==expected.width,"partition count/shape");for(std::size_t row=0;row<got.count;++row){check(got.ticks[row]==expected.ticks[row],"partition sample-time equivalence");for(std::size_t j=0;j<got.width;++j)check(got.data[row*2048+j]==expected.data[row*2048+j],"partition fixed-vector PCM equivalence");}}
 const auto random=partition(0,true);for(std::size_t row=0;row<random.count;++row){check(random.ticks[row]==expected.ticks[row],"random partition time");for(std::size_t j=0;j<random.width;++j)check(random.data[row*2048+j]==expected.data[row*2048+j],"random partition PCM");}
 auto g=short_window();GraphExecutor e(profile());application::PreparedExecution prepared;prepare(e,g,prepared);check(e.apply(std::move(prepared))==Status::ok,"gap initial Apply");Pcm pcm(g);Collected c;
 check(pcm.send(e,256,c,.5f)==Status::ok&&c.count==0,"pre-gap partial hop");
 FrameStamp absent;absent.sequence=0;absent.producer_epoch=17;absent.generation=4;absent.available=false;
 auto missing=NumericView{&pcm.contract,{},pcm.planes,{0,1},{},{}};check(e.process_numeric_ports(1,{absent,missing},{},{collect,nullptr,nullptr,&c})==Status::ok&&!c.available,"unavailable direct source");
 check(pcm.send(e,800,c,-.25f)==Status::ok&&c.count==1,"gap recovery schedule");
 for(std::size_t p=0;p<1;++p)for(std::size_t j=0;j<1024;++j)check(c.data[p*1024+j]==(j<224?0.f:-.25f),"gap cannot retain old PCM");
 node(g,2).enabled=false;prepare(e,g,prepared);check(e.apply(std::move(prepared))==Status::ok,"window bypass Apply");check(pcm.send(e,800,c,.75f)==Status::ok&&c.count==2,"bypass keeps schedule");for(std::size_t j=0;j<c.width;++j)check(c.data[2048+j]==0,"bypass typed zeros");
 node(g,2).enabled=true;prepare(e,g,prepared);check(e.apply(std::move(prepared))==Status::ok,"window resume Apply");check(pcm.send(e,800,c,-.125f)==Status::ok&&c.count==3,"resume schedule");for(std::size_t p=0;p<1;++p)for(std::size_t j=0;j<1024;++j)check(c.data[4096+p*1024+j]==(j<224?0.f:-.125f),"resume excludes bypass PCM");
 for(const Ratio bad:{Ratio{0,1},Ratio{1,0},Ratio{2,120},Ratio{1,1001},Ratio{61,1}}){auto invalid=g;invalid.execution_interval=bad;check(e.prepare_numeric(invalid,{profile().admission,profile().resources},prepared)!=Status::ok&&e.running(),"invalid interval cannot replace active graph");}
 auto low=short_window(30);node(low,1).kind="NetworkInput";node(low,1).parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{30}}}}};std::array<std::size_t,64> workspace{};ProcessingGraphAdmission admission;const auto p=profile();const auto rejected=admit_processing_graph(low,p.admission,PreparedGraph::numeric_capabilities(p),workspace,admission);check(!rejected&&rejected.node==2,"domain rejects sub-sample Window interval");
}
inline NumericProcessingGraph transform_graph(const char* kind,NumericContract input,NumericContract output,NodeId id=5){auto base=oracle_graph();NumericProcessingGraph g;g.graph_id="transform.regression";auto source=node(base,1);source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs[0].contract=own(input);auto transform=node(base,15);transform.id=id;transform.kind=kind;transform.parameters=TransformProcessingParameters{};transform.inputs[0].contract=own(input);transform.outputs[0].contract=own(output);g.nodes={source,transform};g.edges={{1,id,0,0}};return g;}
inline void merge_metadata_extremes_and_bypass(){auto base=oracle_graph();geometry(base,48000,512,4096);const auto mono=std::get<NumericContract>(node(base,15).outputs[0].contract.view());
 auto g=transform_graph("Mixer",mono,mono);auto second=node(g,1);second.id=13;g.nodes.push_back(second);g.edges.push_back({13,5,0,1});
 auto& mixer=node(g,5);mixer.inputs.push_back({1,PortDirection::Input,own(mono)});mixer.parameters=MixerProcessingParameters{};g.configuration_bindings.push_back({16,5});
 ConfiguredProcessingNode clock;clock.id=16;clock.kind="SampleClock";clock.parameters=SampleClockProcessingParameters{};g.nodes.push_back(clock);g.execution_interval={1,60};
 // Descriptive semantic/quantity labels do not gate a numeric Mixer. Real clock,
 // coordinates, dtype and storage requirements do.
 for(int mutation=0;mutation<4;++mutation){auto bad=g;auto c=mono;if(mutation==0)c.clock.sample_rate=44100;if(mutation==1)c.dtype=DType::Float64;if(mutation==2)c.coordinates[0].step={1,1};if(mutation==3)c.coordinates[0].mode=CoordinateMode::ExplicitFloat64LE;node(bad,5).outputs[0].contract=own(c);ResolvedProcessingNode resolved;check(!resolve_processing_node(node(bad,5),profile().admission,resolved),"Mixer real metadata drift rejected");}
 std::array<float,256> data;data.fill(std::numeric_limits<float>::max());PlaneLayout plane{0,{4,0}};
 for(const auto mode:{MixerMode::WeightedMean,MixerMode::WeightedRMS}){std::get<MixerProcessingParameters>(node(g,5).parameters).mode=mode;GraphExecutor e(profile());application::PreparedExecution prepared;prepare(e,g,prepared);check(e.apply(std::move(prepared))==Status::ok,"extreme Mixer Apply");Collected c;c.target=5;FrameStamp stamp;stamp.sequence=stamp.source_sequence=1;stamp.sample_count=800;stamp.has_sample_interval=true;
  const auto before=allocations;auto frame=DataFrameView{stamp,NumericView{&mono,std::as_bytes(std::span(data)),std::span(&plane,1),{256,1},{},{}}};
  check(e.process_numeric_ports(1,frame,{},{collect,nullptr,nullptr,&c})==Status::ok&&e.process_numeric_ports(13,frame,{},{collect,nullptr,nullptr,&c})==Status::ok&&c.count==0,"Mixer inputs queue without hidden output clock");
  check(e.process_clock(16,stamp,{},{collect,nullptr,nullptr,&c})==Status::ok&&c.count==1&&allocations==before,"clocked extreme Mixer success/noalloc");for(std::size_t j=0;j<256;++j)check(c.data[j]==std::numeric_limits<float>::max(),"Mean/RMS FLT_MAX oracle");
 }
 auto identity=transform_graph("SpectrumGain",mono,mono);node(identity,5).parameters=TransformProcessingParameters{{{"gain",NumericLiteral{2.}}}};node(identity,5).enabled=false;GraphExecutor e(profile());application::PreparedExecution prepared;prepare(e,identity,prepared);check(e.apply(std::move(prepared))==Status::ok,"identity bypass Apply");std::array<float,256> values;values.fill(3);PlaneLayout layout{0,{4,0}};FrameStamp stamp;stamp.sequence=1;Collected c;c.target=5;
 auto frame=DataFrameView{stamp,NumericView{&mono,std::as_bytes(std::span(values)),std::span(&layout,1),{256,1},{},{}}};check(e.process_numeric_ports(1,frame,{},{collect,nullptr,nullptr,&c})==Status::ok&&c.data[0]==3,"same-contract bypass forwards instead of computes/mutes");
 node(identity,5).enabled=true;prepare(e,identity,prepared);check(e.apply(std::move(prepared))==Status::ok,"gain resume");frame.stamp.sequence=2;check(e.process_numeric_ports(1,frame,{},{collect,nullptr,nullptr,&c})==Status::ok&&c.data[2048]==6,"active Gain computes");
 auto sink=node(base,8);sink.id=8;sink.kind="NetworkOutput";sink.inputs[0].contract=own(mono);sink.outputs.clear();identity.nodes.push_back(sink);identity.edges.push_back({5,8,0,0});prepare(e,identity,prepared);check(e.apply(std::move(prepared))==Status::ok,"downstream error sink Apply");
 values.fill(std::numeric_limits<float>::max());frame.stamp.sequence=3;c.target=8;check(e.process_numeric_ports(1,frame,{},{nullptr,collect,nullptr,&c})==Status::overflow&&c.last==Status::unavailable&&!c.available,"executor retains transform error while downstream sink is unavailable");
}
inline void normalized_pcm_fft(){auto base=short_window(48000,512,800);const std::array<std::uint32_t,1> channels{0};auto pcm=std::get<NumericContract>(node(base,1).outputs[0].contract.view());pcm.channel_order=channels;pcm.clock.domain="local";auto integer=pcm;integer.dtype=DType::Int16;integer.representation.normalization=Normalization::SignedUnit;integer.representation.alignment=2;pcm.representation.normalization=Normalization::FloatUnit;pcm.unit=NumericUnit::NormalizedAmplitude;
 node(base,1).kind="NetworkInput";node(base,1).parameters=EndpointProcessingParameters{};node(base,1).outputs[0].contract=own(integer);node(base,2).inputs[0].contract=own(pcm);auto window=std::get<NumericContract>(node(base,2).outputs[0].contract.view());window.channel_order=channels;window.clock.domain="local";window.representation.normalization=Normalization::FloatUnit;window.unit=pcm.unit;node(base,2).outputs[0].contract=own(window);
 ConfiguredProcessingNode converter;converter.id=16;converter.kind="Converter";ConversionPolicy policy;policy.scaling.mode=ScaleMode::PcmNormalize;converter.parameters=ConverterProcessingParameters{policy};converter.inputs={{0,PortDirection::Input,own(integer)}};converter.outputs={{0,PortDirection::Output,own(pcm)}};
 auto defaults=oracle_graph();geometry(defaults,48000,512,800);auto fft=node(defaults,4);fft.inputs[0].contract=own(window);base.nodes.push_back(converter);base.nodes.push_back(fft);base.edges={{1,16,0,0},{16,2,0,0},{2,4,0,0}};
 GraphExecutor e(profile());application::PreparedExecution prepared;prepare(e,base,prepared);check(e.apply(std::move(prepared))==Status::ok,"PcmNormalize native FFT pipeline");std::array<std::int16_t,800> samples;samples.fill(-32768);std::array<PlaneLayout,1> planes{{{0,{2,0}}}};FrameStamp stamp;stamp.sequence=1;Collected c;c.target=4;
 check(e.process_numeric_ports(1,{stamp,NumericView{&integer,std::as_bytes(std::span(samples)),planes,{800,1},{},{}}},{},{collect,nullptr,nullptr,&c})==Status::ok&&c.count==1,"normalized PCM FFT emitted");check(std::abs(c.data[0]-1.f)<1e-6,"normalized -1 PCM DC magnitude oracle");
 node(base,4).enabled=false;prepare(e,base,prepared);check(e.apply(std::move(prepared))==Status::ok,"different-contract FFT bypass");stamp.sequence=2;check(e.process_numeric_ports(1,{stamp,NumericView{&integer,std::as_bytes(std::span(samples)),planes,{800,1},{},{}}},{},{collect,nullptr,nullptr,&c})==Status::ok&&c.count==2,"disabled FFT emits typed spectrum");for(std::size_t i=0;i<c.width;++i)check(c.data[2048+i]==0,"different-contract typed spectrum zeros");
}
inline void mel_coordinate_equivalence(){auto defaults=oracle_graph();geometry(defaults,48000,512,4096);const auto input=std::get<NumericContract>(node(defaults,6).inputs[0].contract.view());const auto output=std::get<NumericContract>(node(defaults,6).outputs[0].contract.view());
 std::array<float,256> data;std::array<double,256> frequencies;for(std::size_t i=0;i<data.size();++i){data[i]=float(1+i%13)/17;frequencies[i]=double(i)*48000/512;}PlaneLayout plane{0,{4,0}};
 Collected regular,explicit_result;regular.target=explicit_result.target=5;
 for(unsigned mode=0;mode<2;++mode){auto c=input;if(mode)c.coordinates[0].mode=CoordinateMode::ExplicitFloat64LE;auto g=transform_graph("Mel",c,output);
  NumericArray declared;if(mode)for(const auto hz:frequencies)declared.values.push_back(float(hz));
  node(g,5).parameters=TransformProcessingParameters{{{"bands",NumericLiteral{std::uint64_t(output.shape.extents[0].maximum)}},{"sample_rate",NumericLiteral{std::uint64_t{48000}}},{"bin_hz",NumericLiteral{48000./512.}},{"first_bin",NumericLiteral{std::uint64_t{0}}},{"frequencies_hz",std::move(declared)}}};
  GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"Mel coordinate mode Apply");FrameStamp stamp;stamp.sequence=1;
  NumericView view{&c,std::as_bytes(std::span(data)),std::span(&plane,1),{256,1},{},{}};if(mode)view.coordinates.axes[0]=std::as_bytes(std::span(frequencies));
  auto& result=mode?explicit_result:regular;check(e.process_numeric_ports(1,{stamp,view},{},{collect,nullptr,nullptr,&result})==Status::ok&&result.count==1,"Mel coordinate mode process");
 }
 const auto bands=regular.width;for(std::size_t band=0;band<bands;++band){auto boundary=[&](std::size_t i){return 700*(std::pow(1+24000./700,double(i)/double(bands+1))-1);};const auto left=boundary(band),center=boundary(band+1),right=boundary(band+2);double expected=0;
  for(std::size_t i=0;i<data.size();++i)expected+=data[i]*std::max(0.,std::min((frequencies[i]-left)/(center-left),(right-frequencies[i])/(right-center)));
  check(std::abs(regular.data[band]-expected)<1e-5*std::max(1.,expected)&&std::abs(explicit_result.data[band]-expected)<1e-5*std::max(1.,expected),"regular/explicit independent magnitude-Hz Mel oracle");
 }
}
inline void interval_change_retains_history(){auto g=oracle_graph();geometry(g,48000,512,4096);GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"cadence history Apply");Pcm pcm(g);Collected c;c.target=6;check(pcm.send(e,1600,c)==Status::ok&&rows(e)==2,"initial cadence rows");
 DataFrameView before;check(e.plan()->history(7,0,*e.context(),before)==Status::ok,"history before interval change");const auto clock=std::get<NumericView>(before.payload).contract->clock;const std::string clock_domain(clock.domain);std::array<float,100> values{};const auto& old=std::get<NumericView>(before.payload);for(std::size_t r=0;r<2;++r)for(std::size_t j=0;j<50;++j)check(load_element(old,{0,old.actual[0]-2+r,j},values[r*50+j])==Status::ok,"retained history oracle");
 g.execution_interval={1,30};prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok&&rows(e)==2,"interval Apply keeps compatible History");DataFrameView after;check(e.plan()->history(7,0,*e.context(),after)==Status::ok,"history after interval Apply");const auto& retained=std::get<NumericView>(after.payload);check(retained.contract->clock.domain==clock_domain&&retained.contract->clock.tick_seconds==clock.tick_seconds&&retained.contract->clock.sample_rate==clock.sample_rate,"cadence does not falsify signal clock");for(std::size_t r=0;r<2;++r)for(std::size_t j=0;j<50;++j){float actual=0;check(load_element(retained,{0,retained.actual[0]-2+r,j},actual)==Status::ok&&actual==values[r*50+j],"interval Apply retains history values");}
 const auto count=c.count;check(pcm.send(e,800,c)==Status::ok&&c.count==count&&rows(e)==2,"new interval fresh phase half hop");check(pcm.send(e,800,c)==Status::ok&&c.count==count+1&&rows(e)==3&&c.ticks[count]==3200,"new interval actual sample timestamp");
}
inline void wide_clock_and_expansion_bounds(){auto g=short_window();node(g,1).kind="NetworkInput";node(g,1).parameters=EndpointProcessingParameters{};
 for(auto& n:g.nodes)for(auto* ports:{&n.inputs,&n.outputs})for(auto& port:*ports){auto c=std::get<NumericContract>(port.contract.view());c.clock.tick_seconds={INT64_MAX,1};port.contract=own(c);}
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"wide physical clock Apply");Pcm pcm(g);Collected out;check(pcm.send(e,800,out,.1f)==Status::ok&&out.count==1&&out.ticks[0]==0,"wide clock product divide returns representable zero tick");
 auto bounded=profile();bounded.max_windows_per_ingress=1;GraphExecutor limited(bounded);g=short_window();check(limited.prepare_numeric(g,{bounded.admission,bounded.resources},pending)==Status::exhausted&&!pending,"bounded Window emission expansion admission");
 bounded=profile();bounded.max_callback_deliveries_per_ingress=1;GraphExecutor callbacks(bounded);check(callbacks.prepare_numeric(g,{bounded.admission,bounded.resources},pending)==Status::exhausted&&!pending,"bounded callback expansion admission");
}
inline void detached_preparation_ownership(){auto g=oracle_graph();geometry(g,48000,4096,4096);GraphExecutor e(profile()),foreign(profile());auto empty=e.snapshot_preparation_base();application::PreparedExecution pending;const auto p=profile();check(e.prepare_numeric_detached(g,{p.admission,p.resources},empty,pending)==Status::ok,"detached empty Prepare");check(foreign.register_prepared(pending,empty)==Status::invalid_argument&&bool(pending),"foreign detached registration refused atomically");check(e.register_prepared(pending,empty)==Status::ok&&e.apply(std::move(pending))==Status::ok,"detached owner Apply");
 Pcm pcm(g);Collected out;out.target=6;check(pcm.send(e,1600,out)==Status::ok,"detached base accumulation");auto base=e.snapshot_preparation_base();node(g,9).enabled=false;check(e.prepare_numeric_detached(g,{p.admission,p.resources},base,pending)==Status::ok,"owning detached snapshot preparation");check(pcm.send(e,1600,out)==Status::ok,"processing continues after detached snapshot");const auto latest=rows(e);check(e.register_prepared(pending,base)==Status::ok&&e.apply(std::move(pending))==Status::ok&&rows(e)==latest,"detached Apply refreshes latest compatible history");
 auto stale=e.snapshot_preparation_base();application::PreparedExecution old;check(e.prepare_numeric_detached(g,{p.admission,p.resources},stale,old)==Status::ok,"stale candidate prepared");prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"intervening owner Apply");check(e.register_prepared(old,stale)==Status::would_block&&bool(old)&&e.running(),"stale detached base refused without consumption");
}
struct BurstObservation {
 std::size_t begin=0,count=0;float peak=0;
 float sample(std::int64_t index)const {
  static constexpr std::array<float,4> wave{.25f,0,-.25f,0};
  return index>=std::int64_t(begin)&&index<std::int64_t(begin+96)?wave[std::size_t(index)%4]:0.f;
 }
 double expected(std::int64_t window_begin)const {
  static constexpr std::array<int,4> cosine{1,0,-1,0},sine{0,1,0,-1};double re=0,im=0;
  for(std::size_t j=0;j<512;++j){const auto value=sample(window_begin+std::int64_t(j));re+=value*cosine[j%4];im-=value*sine[j%4];}
  return std::hypot(re,im)/512.;
 }
 static void output(void* user,const NumericPortFrameView& event){
  if(event.identity.node!=4)return;
  auto& self=*static_cast<BurstObservation*>(user);check(event.status==Status::ok&&event.frame.stamp.available,"burst FFT remains available");
  const auto& view=std::get<NumericView>(event.frame.payload);float value=0;
  check(load_element(view,{0,128,0},value)==Status::ok,"read12kHz bin from actual FFT");
  check(event.frame.stamp.has_sample_interval&&event.frame.stamp.sample_count==512,"burst output declares exact512-sample aperture");
  check(std::abs(value-self.expected(event.frame.stamp.sample_begin))<1e-6,"burst every-window bin matches independent direct DFT");
  self.peak=std::max(self.peak,value);++self.count;
 }
};
struct BurstStream {
 const NumericContract& contract;BurstObservation& observation;std::size_t sent=0,sequence=0;std::array<float,256> samples{};std::array<PlaneLayout,1> planes{{{0,{4,0}}}};
 void send(GraphExecutor& executor,std::size_t count){
  for(std::size_t i=0;i<count;++i)samples[i]=observation.sample(std::int64_t(sent+i));
  FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=1;stamp.generation=1;
  stamp.timestamp_ticks=stamp.sample_begin=std::int64_t(sent);stamp.sample_count=count;stamp.has_sample_interval=true;
  const DataFrameView frame{stamp,NumericView{&contract,std::as_bytes(std::span(samples)),planes,{count,1},{},{}}};
  const auto before=allocations;
  check(executor.process_numeric_ports(1,frame,{},{BurstObservation::output,nullptr,nullptr,&observation})==Status::ok&&allocations==before,"burst partition processing has no allocation");
  sent+=count;
 }
};
inline void burst_partition(std::size_t begin,std::size_t partition){
 auto graph=short_window(48000,512,256);graph.execution_interval={1,120};
 auto templates=oracle_graph();geometry(templates,48000,512,256);graph.nodes.push_back(node(templates,4));graph.edges.push_back({2,4,0,0});
 GraphExecutor executor(profile());application::PreparedExecution pending;prepare(executor,graph,pending);
 check(executor.apply(std::move(pending))==Status::ok,"burst explicit mono Window/FFT Apply");
 auto contract=std::get<NumericContract>(node(graph,1).outputs[0].contract.view());BurstObservation observation{begin};BurstStream stream{contract,observation};
 while(stream.sent<1600)stream.send(executor,std::min(partition,1600-stream.sent));
 check(observation.count==4&&observation.peak>.02f,"2ms12kHz burst survives120Hz analysis at every hop phase");
}
inline void short_high_frequency_burst(){
 for(const auto begin:{399u,401u,650u})for(const auto partition:{127u,256u})burst_partition(begin,partition);
}
inline void run(){short_high_frequency_burst();interval_partition_gap_bypass();merge_metadata_extremes_and_bypass();normalized_pcm_fft();mel_coordinate_equivalence();interval_change_retains_history();wide_clock_and_expansion_bounds();detached_preparation_ownership();}
}
