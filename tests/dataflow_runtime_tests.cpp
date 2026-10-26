#include "canonical_runtime_test_fixture.h"
#include "canonical_core_regressions.h"
#include <bit>
#include <domain/signal/contracts.h>
#ifdef BARELIEF_TEST_DESKTOP_SPEEX
#include <speex_backend.h>
#endif
using namespace canonical_test;
static std::array<std::byte,50*8> mel_coordinates(){
 std::array<std::byte,50*8> bytes{};
 for(std::size_t band=0;band<50;++band){const auto bits=std::bit_cast<std::uint64_t>(double(band+1)*100.);for(unsigned byte=0;byte<8;++byte)bytes[band*8+byte]=std::byte((bits>>(byte*8))&255);}
 return bytes;
}
struct Spectra{std::array<std::size_t,2> count{};std::array<std::int64_t,2> end{};std::array<std::size_t,2> peak{};};
static std::size_t spectrum_peak(const NumericView& view){std::size_t peak=0;float maximum=-1;
 for(std::size_t i=0;i<view.actual[0];++i){float value=0;
  check(load_element(view,{0,i,0},value)==Status::ok&&std::isfinite(value),"FFT finite");
  if(value>maximum){maximum=value;peak=i;}}
 return peak;
}
static void spectrum(void* user,const NumericPortFrameView& f){
 if(f.status!=Status::ok || (f.identity.node!=4&&f.identity.node!=24))return;
 auto& result=*static_cast<Spectra*>(user);const auto& view=std::get<NumericView>(f.frame.payload);
 check(view.planes.size()==1,"mono FFT output");const auto slot=f.identity.node==4?0:1;
 result.peak[slot]=spectrum_peak(view);++result.count[slot];result.end[slot]=f.frame.stamp.timestamp_ticks;
}
static NumericProcessingGraph single_window_graph(std::size_t size){
 auto graph=oracle_graph();std::erase_if(graph.nodes,[](const auto& n){return n.id>4 || n.id==3;});
 graph.configuration_bindings.clear();graph.edges={{1,2,0,0},{2,4,0,0}};geometry(graph,8192,size,256);return graph;
}
static NumericProcessingGraph dual_window_graph(){auto graph=single_window_graph(2048),short_graph=single_window_graph(512);
 for(auto n:short_graph.nodes)if(n.id!=1){n.id+=20;graph.nodes.push_back(std::move(n));}
 graph.edges.push_back({1,22,0,0});graph.edges.push_back({22,24,0,0});return graph;
}
static void independent_windows(){auto g=dual_window_graph();
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"apply dual windows");Feed feed(g);Spectra result;
 for(int i=0;i<32;++i)feed.process(e,256,{spectrum,nullptr,nullptr,&result});
 for(std::size_t i=0;i<2;++i)check(result.count[i]==60&&result.end[i]==8192&&result.peak[i]==(i==0?128:32),"independent mono window schedule/timestamp");
 std::puts("independent windows2048/512 mono peaks128/32 PASS");}
struct RetainedHistory {std::size_t filled=0,bands=0;std::uint64_t sequence=0;bool discontinuity=false;std::vector<std::uint64_t> bits;};
static RetainedHistory retained_history(const GraphExecutor& e,NodeId id=7){DataFrameView frame;std::uint64_t filled=0;check(e.plan()->history(id,0,*e.context(),frame,&filled)==Status::ok,"retain History observation");const auto& view=std::get<NumericView>(frame.payload);RetainedHistory result;result.sequence=frame.stamp.sequence;result.discontinuity=frame.stamp.discontinuity;result.filled=std::size_t(filled);result.bands=view.actual[1];result.bits.resize(result.filled*result.bands);
 for(std::size_t r=0;r<result.filled;++r)for(std::size_t b=0;b<result.bands;++b)check(load_bits(view,{0,view.actual[0]-result.filled+r,b},view.contract->dtype,result.bits[r*result.bands+b])==Status::ok,"retain History exact bits");return result;}
static void appended_history(const RetainedHistory& before,const GraphExecutor& e,NodeId id=7){const auto after=retained_history(e,id);check(after.filled==before.filled+1&&after.bands==before.bands,"compatible source boundary appends one History row");check(std::equal(before.bits.begin(),before.bits.end(),after.bits.begin()),"compatible source boundary preserves all old History bits");if(!after.discontinuity || after.sequence!=before.sequence+1)std::fprintf(stderr,"History boundary before=%llu after=%llu discontinuity=%d\n",(unsigned long long)before.sequence,(unsigned long long)after.sequence,int(after.discontinuity));check(after.discontinuity&&after.sequence==before.sequence+1,"compatible source boundary marks discontinuity without restarting History sequence");}
static void pending_apply_reads_latest(GraphExecutor& e,Feed& feed,const NumericProcessingGraph& graph,application::PreparedExecution& pending){
 auto changed=graph;node(changed,9).enabled=false;prepare(e,changed,pending);
 for(int i=0;i<8;++i)feed.process(e,256);
 const auto before=rows(e);check(e.apply(std::move(pending))==Status::ok&&rows(e)==before,"Apply copied stale Prepare history");
}
static void shutdown_fences_pending(GraphExecutor& e,Feed& feed,const NumericProcessingGraph& graph,application::PreparedExecution& pending){
 prepare(e,graph,pending);check(e.shutdown()==Status::ok,"shutdown pending");
 check(e.apply(std::move(pending))==Status::invalid_argument&&!e.running(),"shutdown fences pending Apply");
 check(restart(e,graph)==Status::ok,"re-admit after shutdown");
 for(int i=0;i<4;++i)feed.process(e,256);
 check(rows(e)==1,"shutdown resurrected old History");
}
static void foreign_apply_and_epoch(GraphExecutor& e,Feed& feed,const NumericProcessingGraph& graph,application::PreparedExecution& pending){
 prepare(e,graph,pending);GraphExecutor foreign(profile());
 check(foreign.apply(std::move(pending))==Status::invalid_argument&&bool(pending),"foreign executor consumes bundle");
 check(e.apply(std::move(pending))==Status::ok,"owner Apply after foreign reject");
 for(int i=0;i<8;++i)feed.process(e,256);
 check(rows(e)>1,"restart accumulation");const auto before_epoch=retained_history(e);++feed.epoch;
 for(int i=0;i<4;++i)feed.process(e,256);
 appended_history(before_epoch,e);
}
static void latest_state_at_apply(){auto g=oracle_graph();GraphExecutor e(profile());application::PreparedExecution pending;
 prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"initial apply");Feed feed(g);
 for(int i=0;i<32;++i)feed.process(e,256);
 pending_apply_reads_latest(e,feed,g,pending);shutdown_fences_pending(e,feed,g,pending);
 foreign_apply_and_epoch(e,feed,g,pending);
 std::puts("late Apply/latest state, shutdown fence, foreign bundle, compatible producer History retention PASS");}
static NumericProcessingGraph dual_branch_graph(const NumericProcessingGraph& base){auto graph=base;
 for(auto copy:base.nodes){copy.id+=20;graph.nodes.push_back(std::move(copy));}
 for(auto edge:base.edges){edge.from+=20;edge.to+=20;graph.edges.push_back(edge);}
 return graph;
}
static void resize_history_contract(NumericProcessingGraph& graph){
 auto& history=node(graph,7);std::get<HistoryProcessingParameters>(history.parameters).capacity=200;
 for(auto* ports:{&history.outputs,&node(graph,8).inputs})for(auto& port:*ports){
  auto contract=std::get<NumericContract>(port.contract.view());
  if(contract.semantic!=Semantic::History)continue;
  contract.shape.extents[0]={0,200};
  check(own_processing_contract(contract,limits,65536,port.contract)==Status::ok,"resize History contract");
 }
}
static std::array<std::uint64_t,50> newest_history_bits(const GraphExecutor& executor){
 DataFrameView snapshot;check(executor.plan()->history(7,0,*executor.context(),snapshot)==Status::ok,"resize newest baseline");
 const auto& view=std::get<NumericView>(snapshot.payload);std::array<std::uint64_t,50> bits{};
 for(std::size_t band=0;band<bits.size();++band)
  check(load_bits(view,{0,view.actual[0]-1,band},DType::Float32,bits[band])==Status::ok,"resize baseline bits");
 return bits;
}
static void check_newest_history_bits(const GraphExecutor& executor,const std::array<std::uint64_t,50>& expected){
 DataFrameView snapshot;check(executor.plan()->history(7,0,*executor.context(),snapshot)==Status::ok,"resize retained snapshot");
 const auto& view=std::get<NumericView>(snapshot.payload);
 for(std::size_t band=0;band<expected.size();++band){std::uint64_t actual=0;
  check(load_bits(view,{0,view.actual[0]-1,band},DType::Float32,actual)==Status::ok&&actual==expected[band],"capacity Apply changed freshest bits");}
}
struct SelectiveResetFixture {
 NumericProcessingGraph base=oracle_graph(),graph=dual_branch_graph(base);
 GraphExecutor executor{profile()};application::PreparedExecution pending;
 Feed left{base},right{base};
 void process_right(){auto frame=right.frame(256);const auto before=allocations;
  check(executor.process_numeric_ports(21,frame,{}, {})==Status::ok&&allocations==before,"right branch process/noalloc");}
 void start(){prepare(executor,graph,pending);const auto admission=pending.plan->numeric_admission();
  check(admission && admission->resources.payload_bytes>=2*2048*50*sizeof(double) && admission->resources.payload_bytes<=profile().resources.maximum.payload_bytes,"both retained Mel plans charged within bounded two-branch payload budget");
  check(executor.apply(std::move(pending))==Status::ok,"two branches Apply");
  for(int i=0;i<32;++i){left.process(executor,256);process_right();}
  check(rows(executor,27)==rows(executor,7),"two branches counts");}
 void restart_source(){const auto original=rows(executor,7);const auto before=retained_history(executor,27);
  ++right.epoch;for(int i=0;i<4;++i)process_right();
  appended_history(before,executor,27);check(rows(executor,7)==original,"source restart reset independent branch");}
 void rebind_clock(){for(auto& edge:graph.edges)if(edge.to==7)edge.from=26;
  const auto untouched=rows(executor,27);prepare(executor,graph,pending);
  check(executor.apply(std::move(pending))==Status::ok&&rows(executor,27)==untouched,"clock edit reset independent History");
  for(int i=0;i<4;++i)process_right();
  check(rows(executor,7)==1&&rows(executor,27)==untouched+1,"source identity change failed selective reset");}
 void preserve_capacity(){resize_history_contract(graph);const auto independent=rows(executor,27),retained=rows(executor,7);
  const auto bits=newest_history_bits(executor);prepare(executor,graph,pending);
  check(executor.apply(std::move(pending))==Status::ok&&rows(executor,27)==independent&&rows(executor,7)==retained,"capacity Apply changed retained occupancy or independent branch");
  check_newest_history_bits(executor,bits);for(int i=0;i<4;++i)process_right();
  check(rows(executor,7)==retained+1&&rows(executor,27)==independent+1,"capacity edit reset accumulation");}
};
static void selective_reset(){SelectiveResetFixture fixture;
 fixture.start();fixture.restart_source();fixture.rebind_clock();fixture.preserve_capacity();
 std::puts("source restart/rebind selective reset and capacity preservation PASS");}
static NumericProcessingGraph marker_graph(){auto graph=oracle_graph();auto input=node(graph,6).outputs[0].contract;
 auto& source=node(graph,1);source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs[0].contract=input;
 std::erase_if(graph.nodes,[](const auto& configured){return configured.id!=1&&configured.id!=7;});
 graph.configuration_bindings.clear();graph.edges={{1,7,0,0}};return graph;}
struct SourceFenceFixture {
 NumericProcessingGraph graph=marker_graph();GraphExecutor executor{profile()};Feed feed{graph};
 std::array<std::byte,50*8> frequencies=mel_coordinates();
 SourceFenceFixture(){application::PreparedExecution pending;prepare(executor,graph,pending);
  check(executor.apply(std::move(pending))==Status::ok,"marker Apply");}
 Status send(std::uint64_t epoch,std::uint64_t generation,std::uint64_t sequence,bool discontinuity=false,bool available=true){
  auto frame=feed.frame(50);auto& numeric=std::get<NumericView>(frame.payload);
  numeric.planes=std::span(feed.planes).first(1);numeric.coordinates.axes[0]=frequencies;
  frame.stamp.producer_epoch=epoch;frame.stamp.generation=generation;frame.stamp.sequence=sequence;
  frame.stamp.discontinuity=discontinuity;frame.stamp.available=available;
  if(!available)numeric.storage={};
  const auto before=allocations;const auto status=executor.process_numeric_ports(1,frame,{});
  check(allocations==before,"marker hot allocation");return status;
 }
 void generation_transition(){auto before=retained_history(executor);
  check(send(17,1,1)==Status::ok,"generation transition sequence restart");appended_history(before,executor);
  check(send(17,0,102)==Status::would_block&&rows(executor)==3,"old generation accepted");}
 void producer_transition(){auto before=retained_history(executor);
  check(send(99,1,1)==Status::ok,"new producer sequence restart");appended_history(before,executor);
  check(send(17,1,103)==Status::would_block&&rows(executor)==4,"retired producer accepted");}
 void discontinuity(){auto before=retained_history(executor);
  check(send(99,1,2,true)==Status::ok,"explicit discontinuity");appended_history(before,executor);
  check(send(99,1,2,true)==Status::would_block&&rows(executor)==5,"duplicate discontinuity mutates History");}
 void unavailable_marker(){auto before=retained_history(executor);
  check(send(99,2,0,false,false)==Status::ok,"unavailable generation marker");
  check(send(99,1,3)==Status::would_block,"old data revived marker");
  check(send(99,2,3)==Status::ok,"current generation recovery");appended_history(before,executor);}
};
static void source_fences(){SourceFenceFixture fixture;
 check(fixture.send(17,0,100)==Status::ok&&fixture.send(17,0,101)==Status::ok&&rows(fixture.executor)==2,"generation zero baseline");
 fixture.generation_transition();fixture.producer_transition();fixture.discontinuity();fixture.unavailable_marker();
 std::puts("generation0/restart/retired producer/duplicate discontinuity/unavailable fences PASS");}
// Retained ring and restart predicates from the old mixed runtime_tests fixture.
// Capacity edits preserve newest real rows and extend only the older typed-zero tail.
struct RingCapacityFixture {
 NumericProcessingGraph graph=oracle_graph();GraphExecutor executor{profile()};application::PreparedExecution pending;
 NumericContract contract;std::array<float,50> values{};PlaneLayout plane{0,{4,0}};
 std::uint64_t sequence=0,expected_ordinal=0;
 std::array<std::byte,50*8> frequency_bytes=mel_coordinates();
 RingCapacityFixture(){auto retained=node(graph,6).outputs[0].contract;
  auto& source=node(graph,1);source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs[0].contract=retained;
  std::erase_if(graph.nodes,[](const auto& n){return n.id!=1&&n.id!=7;});
  graph.configuration_bindings.clear();graph.edges={{1,7,0,0}};
  capacity(400);prepare(executor,graph,pending);check(executor.apply(std::move(pending))==Status::ok,"ring Apply");
  contract=std::get<NumericContract>(node(graph,1).outputs[0].contract.view());
 }
 void capacity(std::size_t count){auto& history=node(graph,7);
  std::get<HistoryProcessingParameters>(history.parameters).capacity=count;
  auto value=std::get<NumericContract>(history.outputs[0].contract.view());value.shape.extents[0]={0,count};
  check(own_processing_contract(value,limits,65536,history.outputs[0].contract)==Status::ok,"ring capacity contract");
 }
 DataFrameView input(){values.fill(float(++sequence));FrameStamp stamp;
  stamp.sequence=sequence;stamp.source_sequence=sequence;stamp.producer_epoch=21;stamp.generation=3;stamp.timestamp_ticks=std::int64_t(sequence);
  return {stamp,NumericView{&contract,std::as_bytes(std::span(values)),std::span(&plane,1),{50,1},{},CoordinateView{{std::span<const std::byte>(frequency_bytes),{}}}}};
 }
 void feed(){++expected_ordinal;auto frame=input();const auto before=allocations;
  check(executor.process_numeric_ports(1,frame,{})==Status::ok&&allocations==before,"ring process noalloc");
 }
 void check_padding(const NumericView& view,std::uint64_t filled){
  for(std::size_t row=0;row<view.actual[0]-filled;++row)
   for(std::size_t band=0;band<50;++band){float value=1;
    check(load_element(view,{0,row,band},value)==Status::ok&&value==0,"fixed zero old tail");}
 }
 void check_coordinates(const NumericView& view,std::uint64_t filled){
  check(view.contract->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && view.contract->coordinates[0].unit==NumericUnit::Seconds && view.coordinates.axes[0].size()==view.actual[0]*8,"ring explicit source-clock times");
  for(std::size_t byte=0;byte<(view.actual[0]-filled)*8;++byte)
   check(view.coordinates.axes[0][byte]==std::byte{0},"zero older padding must not fabricate observation times");
 }
 void check_rows(const NumericView& view,std::uint64_t filled,std::initializer_list<float> expected){
  std::size_t row=view.actual[0]-filled;
  for(float want:expected){
   for(std::size_t band=0;band<50;++band){float actual=0;
    check(load_element(view,{0,row,band},actual)==Status::ok&&actual==want,"ring logical order/write position");}
   std::uint64_t bits=0;
   for(unsigned byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned>(view.coordinates.axes[0][row*8+byte]))<<(8*byte);
   check(std::abs(std::bit_cast<double>(bits)-double(want)/48000.)<1e-15,"ring times survive wrap/compatible Apply/reset exactly with values");
   ++row;
  }
 }
 void exact(std::initializer_list<float> expected){DataFrameView frame;std::uint64_t filled=0,ordinal=0;
  const auto before=allocations;
  check(executor.plan()->history(7,0,*executor.context(),frame,&filled,&ordinal)==Status::ok&&allocations==before,"ring query noalloc");
  const auto view=std::get<NumericView>(frame.payload);
  check(validate_view(frame,limits)==Status::ok && view.actual[0]==std::get<HistoryProcessingParameters>(node(graph,7).parameters).capacity&&filled==expected.size()&&view.actual[1]==50,"fixed ring dimensions/real occupancy");
  check(ordinal==expected_ordinal,"resize preserves accepted ordinal");
  check_padding(view,filled);check_coordinates(view,filled);check_rows(view,filled,expected);
 }
 void apply(const char* reason){prepare(executor,graph,pending);
  check(executor.apply(std::move(pending))==Status::ok,reason);
 }
 void empty_resizes(){exact({});capacity(4);apply("empty History resize Apply");exact({});
  capacity(8);apply("empty History expansion Apply");exact({});
  capacity(4);apply("empty History shrink Apply");exact({});
 }
 void wrap_at_four(){for(int i=0;i<7;++i)feed();exact({4,5,6,7});
  apply("full ring compatible Apply");exact({4,5,6,7});feed();exact({5,6,7,8});
 }
 void grow_to_eight(){capacity(8);apply("grow compatible Apply");exact({5,6,7,8});
  feed();exact({5,6,7,8,9});feed();exact({5,6,7,8,9,10});
 }
 void shrink_to_two(){capacity(2);apply("shrink compatible Apply");exact({9,10});
  feed();exact({10,11});feed();exact({11,12});feed();exact({12,13});
 }
 void restart_cycles(){for(int cycle=0;cycle<10;++cycle){
   check(executor.shutdown()==Status::ok&&executor.shutdown()==Status::ok&&!executor.running(),"repeated owner shutdown");
   auto frame=input();check(executor.process_numeric_ports(1,frame,{})==Status::would_block,"shutdown graph delivered");
   check(restart(executor,graph)==Status::ok&&restart(executor,graph)==Status::ok,"repeated re-admission");
   expected_ordinal=0;feed();exact({float(sequence)});feed();exact({float(sequence-1),float(sequence)});
  }
 }
};
static void ring_capacity_and_restart(){RingCapacityFixture fixture;
 fixture.empty_resizes();fixture.wrap_at_four();fixture.grow_to_eight();fixture.shrink_to_two();fixture.restart_cycles();
 std::puts("mixed-runtime ring wrap/compatible Apply/explicit grow-shrink reset/write position/10 restart cycles PASS");
}
static void check_restarted_mel(GraphExecutor& executor){DataFrameView snapshot;
 check(executor.plan()->history(7,0,*executor.context(),snapshot)==Status::ok,"DSP restart snapshot");
 const auto& view=std::get<NumericView>(snapshot.payload);bool positive=false;
 for(std::size_t band=0;band<50;++band){float value=0;
  check(load_element(view,{0,view.actual[0]-1,band},value)==Status::ok&&std::isfinite(value)&&value>=0,"DSP restart Mel finite/nonnegative");
  positive|=value>0;}
 check(positive,"DSP restart did not resume real Mel");}
static void restart_dsp_cycle(GraphExecutor& executor,const NumericProcessingGraph& graph,Feed& feed){
 check(executor.shutdown()==Status::ok&&executor.shutdown()==Status::ok,"DSP repeated owner shutdown");
 auto blocked=feed.frame(256);check(executor.process_numeric_ports(1,blocked,{})==Status::would_block,"shutdown DSP accepts input");
 check(restart(executor,graph)==Status::ok&&restart(executor,graph)==Status::ok,"DSP repeated re-admission");
 for(int i=0;i<4;++i)feed.process(executor,256);
 check(rows(executor)==1,"DSP restart retained old History/cadence");check_restarted_mel(executor);}
static void repeated_dsp_restart(){auto graph=oracle_graph();GraphExecutor executor(profile());
 application::PreparedExecution pending;prepare(executor,graph,pending);
 check(executor.apply(std::move(pending))==Status::ok,"DSP restart Apply");Feed feed(graph);
 for(int cycle=0;cycle<10;++cycle)restart_dsp_cycle(executor,graph,feed);
 std::puts("actual 8-tone Window/FFT/Mel/History repeated re-admission/Stop10 cycles, fresh positive finite output PASS");}
struct SelectorObservation {
 std::size_t available=0,unavailable=0,windows=0;
 FrameStamp last{};bool inspect_fresh_window=false,expect_history_discontinuity=false,expect_selector_zero=false;
 std::uint64_t history_sequence=0;
};
static void check_selector_zeros(const NumericView& view){
 for(std::size_t plane=0;plane<view.planes.size();++plane)
  for(std::size_t sample=0;sample<view.actual[0];++sample){float value=1;
   check(load_element(view,{plane,sample,0},value)==Status::ok&&value==0,
    "disabled selector did not emit typed PCM zeros");}
}
static void observe_selector(SelectorObservation& observation,const NumericPortFrameView& frame){
 observation.last=frame.frame.stamp;
 if(frame.frame.stamp.available){++observation.available;
  check(frame.status==Status::ok,"selector available status");
  if(observation.expect_selector_zero)check_selector_zeros(std::get<NumericView>(frame.frame.payload));
 }else{++observation.unavailable;
  check(frame.status==Status::unavailable&&std::get<NumericView>(frame.frame.payload).storage.empty(),
   "selector unavailable is not available zeros");}
}
static void check_fresh_window(const NumericView& view){
 for(std::size_t plane=0;plane<view.planes.size();++plane)
  for(std::size_t sample=0;sample<view.actual[0]-800;++sample){float value=1;
   check(load_element(view,{plane,sample,0},value)==Status::ok&&value==0,
    "selector mixed old producer into fresh Window");}
}
static void observe_window(SelectorObservation& observation,const NumericPortFrameView& frame){
 ++observation.windows;
 if(!observation.inspect_fresh_window)return;
 check_fresh_window(std::get<NumericView>(frame.frame.payload));
 check(frame.frame.stamp.discontinuity,"first Window after selector switch lacks discontinuity");
 observation.inspect_fresh_window=false;
}
static void observe_selector_history(SelectorObservation& observation,const NumericPortFrameView& frame){
 check(frame.frame.stamp.sequence==observation.history_sequence+1,"selector History sequence lost continuity");
 observation.history_sequence=frame.frame.stamp.sequence;
 if(!observation.expect_history_discontinuity)return;
 check(frame.frame.stamp.discontinuity&&frame.frame.stamp.producer_epoch==observation.last.producer_epoch&&
  frame.frame.stamp.source_sequence==observation.last.source_sequence&&
  frame.frame.stamp.timestamp_ticks>=observation.last.timestamp_ticks,
  "selector appended History discontinuity/source/timestamp lost");
 observation.expect_history_discontinuity=false;
}
static void selector_output(void* user,const NumericPortFrameView& frame){
 auto& observation=*static_cast<SelectorObservation*>(user);
 if(frame.identity.node==14)observe_selector(observation,frame);
 if(frame.identity.node==2&&frame.frame.stamp.available)observe_window(observation,frame);
 if(frame.identity.node==7&&frame.identity.port==0&&frame.frame.stamp.available)
  observe_selector_history(observation,frame);
}
// The fixture keeps the prepared graph and both ingress clocks across selector edits.
struct InputSelectorFixture {
 NumericProcessingGraph g=oracle_graph();OwnedProcessingContract pcm=node(g,1).outputs[0].contract;
 GraphExecutor e{profile()};application::PreparedExecution pending;Feed generator{g},capture{g};
 SelectorObservation o;std::size_t retained=0;std::vector<std::uint64_t> bits;
 InputSelectorFixture(){auto physical=node(g,1);physical.id=13;physical.kind="AudioSource";
  physical.parameters=EndpointProcessingParameters{};g.nodes.push_back(std::move(physical));
  ConfiguredProcessingNode selector;selector.id=14;selector.kind="InputSelector";
  selector.parameters=TransformProcessingParameters{};
  selector.inputs={{0,PortDirection::Input,pcm},{1,PortDirection::Input,pcm}};
  selector.outputs={{0,PortDirection::Output,pcm}};g.nodes.push_back(std::move(selector));
  for(auto& edge:g.edges)if(edge.from==1&&edge.output_port==0)edge.from=14;
  g.edges.push_back({1,14,0,0});g.edges.push_back({13,14,0,1});
  prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"selector initial Apply");
  capture.epoch=99;capture.sent=480000;
 }
 FrameStamp send(NodeId id,Feed& feed,bool available=true,bool discontinuity=false){
  auto frame=feed.frame(256);frame.stamp.available=available;frame.stamp.discontinuity=discontinuity;
  if(!available)std::get<NumericView>(frame.payload).storage={};
  const auto before=allocations;
  check(e.process_numeric_ports(id,frame,{}, {selector_output,nullptr,nullptr,&o})==Status::ok,"selector ingress");
  check(allocations==before,"selector hot allocation");return frame.stamp;
 }
 void apply(const char* reason){prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,reason);}
 void select(std::uint64_t input){std::get<TransformProcessingParameters>(node(g,14).parameters).values={{"selected_input",NumericLiteral{input}}};apply("selector compatible Apply");}
 void capture_history(){DataFrameView old;check(e.plan()->history(7,0,*e.context(),old)==Status::ok,"selector old History");
  const auto& view=std::get<NumericView>(old.payload);
  for(std::size_t row=0;row<retained;++row)for(std::size_t band=0;band<50;++band){std::uint64_t value=0;
   check(load_bits(view,{0,view.actual[0]-retained+row,band},DType::Float32,value)==Status::ok,"selector History bits");bits.push_back(value);}
 }
 void baseline(){for(int i=0;i<32;++i){send(1,generator);send(13,capture);}
  retained=rows(e);check(retained>0&&o.available==32,"selector default input0");capture_history();}
 void unused_port_edits(){node(g,14).inputs.push_back({2,PortDirection::Input,pcm});apply("unused selector input growth Apply");
  const auto sequence=o.last.sequence;send(1,generator);
  check(o.last.sequence==sequence+1&&!o.last.discontinuity&&rows(e)==retained,"unused input growth reset active selector clock or History");
  node(g,14).inputs.pop_back();apply("unused selector input shrink Apply");
  const auto chosen=o.available;send(13,capture,false);++capture.epoch;send(13,capture,true,true);
  check(o.available==chosen&&o.unavailable==0&&rows(e)==retained,"inactive source poisoned selected History");}
 void preserved_history(){DataFrameView frame;check(e.plan()->history(7,0,*e.context(),frame)==Status::ok,"selector retained History query");
  const auto& view=std::get<NumericView>(frame.payload);
  for(std::size_t row=0;row<retained;++row)for(std::size_t band=0;band<50;++band){std::uint64_t value=0;
   check(load_bits(view,{0,view.actual[0]-(retained+1)+row,band},DType::Float32,value)==Status::ok&&value==bits[row*50+band],"selector mutated retained History bits");}
 }
 void switch_active(){select(1);check(rows(e)==retained,"selector Apply cleared History");
  o.inspect_fresh_window=true;o.expect_history_discontinuity=true;
  const auto first=send(13,capture);
  check(o.last.discontinuity&&o.last.producer_epoch==first.producer_epoch&&o.last.source_sequence==first.source_sequence&&o.last.timestamp_ticks==first.timestamp_ticks,"selector switch metadata");
  for(int i=0;i<3;++i)send(13,capture);
  check(!o.inspect_fresh_window&&!o.expect_history_discontinuity&&rows(e)==retained+1,"selector switch fresh DSP row");preserved_history();
 }
 void recover_selected(){
  const auto active=o.available;for(int i=0;i<4;++i)send(1,generator);
  check(o.available==active,"inactive generator reached selector output");
  send(13,capture,false);DataFrameView unavailable;std::uint64_t filled=0;
  check(o.unavailable==1&&e.plan()->history(7,0,*e.context(),unavailable,&filled)==Status::unavailable&&filled==retained+1,"selected unavailable lost retained History");
  o.inspect_fresh_window=true;o.expect_history_discontinuity=true;
  for(int i=0;i<4;++i)send(13,capture);
  check(!o.inspect_fresh_window&&!o.expect_history_discontinuity&&rows(e)==retained+2,"selector recovery DSP");
 }
 void disable_and_resume(){select(0);const auto resumed=send(1,generator);
  check(o.last.discontinuity&&o.last.producer_epoch==generator.epoch&&o.last.sequence==resumed.sequence,"selector generator did not advance independently");
  node(g,14).enabled=false;apply("disabled selector Apply");o.expect_selector_zero=true;
  auto active=o.available;send(1,generator);
  check(o.available==active+1&&o.last.available,"disabled selector became unavailable with healthy clock");
  active=o.unavailable;send(1,generator,false);
  check(o.unavailable==active+1&&!o.last.available,"disabled selector hid unavailable active source");
  node(g,14).enabled=true;o.expect_selector_zero=false;
 }
 std::uint64_t filled_history(){DataFrameView frame;std::uint64_t filled=0;
  const auto status=e.plan()->history(7,0,*e.context(),frame,&filled);
  check(status==Status::ok||status==Status::unavailable,"N selector retained unavailable History query");return filled;}
 void grow_and_reject(){node(g,14).inputs.push_back({2,PortDirection::Input,pcm});g.edges.push_back({1,14,0,2});
  const auto prior=filled_history();select(2);check(filled_history()==prior,"N selector growth retains unavailable History");
  const auto active=o.available;send(1,generator);check(o.available==active+1,"N selector third stable input delivers");
  node(g,14).inputs.pop_back();std::erase_if(g.edges,[](const auto& edge){return edge.to==14&&edge.input_port==2;});select(0);
  auto malformed=g;std::get<TransformProcessingParameters>(node(malformed,14).parameters).values={{"selected_input",NumericLiteral{std::uint64_t(2)}}};
  auto p=profile();check(e.prepare_numeric(malformed,{p.admission,p.resources},pending)!=Status::ok,"selector accepted input2");
  malformed=g;node(malformed,14).inputs[1].contract=node(g,6).outputs[0].contract;
  check(e.prepare_numeric(malformed,{p.admission,p.resources},pending)!=Status::ok,"selector accepted incompatible input");
 }
 NumericContract disconnect_selected_edge(){select(1);std::erase_if(g.edges,[](const auto& edge){return edge.to==14&&edge.input_port==1;});
  auto unrelated=node(g,13);unrelated.id=16;auto contract=std::get<NumericContract>(pcm.view());
  contract.clock={"unrelated",{1,44100},44100};
  std::get<EndpointProcessingParameters>(unrelated.parameters).values.push_back({"sample_rate",NumericLiteral{std::uint64_t{44100}}});
  check(own_processing_contract(contract,limits,65536,unrelated.outputs[0].contract)==Status::ok,"unrelated ingress contract");
  check(numeric_metadata_output_contract(contract,true,unrelated.outputs.back().contract)==Status::ok,"unrelated declared Fs port matches its own source clock");
  g.nodes.push_back(std::move(unrelated));apply("selector missing selected edge Apply");o.expect_selector_zero=true;return contract;
 }
 void disconnected_clock(Feed& unrelated){const auto active=o.available;
  send(1,generator);send(16,unrelated);check(o.available==active,"disconnected selector ticked on inactive/unrelated clock");
  const auto silence=send(13,capture);
  check(o.available==active+1&&o.last.available&&o.last.producer_epoch==silence.producer_epoch&&o.last.timestamp_ticks==silence.timestamp_ticks,"disconnected selected edge known PCM clock/provenance");
  DataFrameView history;std::uint64_t filled=0;const auto status=e.plan()->history(7,0,*e.context(),history,&filled);
  check(status==Status::ok||status==Status::unavailable,"disconnected selector History retention");
  const auto prior=std::size_t(filled);
  for(int i=0;i<3;++i){send(1,generator);send(16,unrelated);send(13,capture);}
  check(o.available==active+4&&rows(e)==prior+1,"disconnected selector double cadence on alternating sources");
 }
 void cold_disconnected_clock(Feed& unrelated){GraphExecutor cold(profile());prepare(cold,g,pending);
  check(cold.apply(std::move(pending))==Status::ok,"cold disconnected selector Apply");
  SelectorObservation observed;observed.expect_selector_zero=true;
  const auto cold_send=[&](NodeId id,Feed& feed){auto frame=feed.frame(256);const auto before=allocations;
   check(cold.process_numeric_ports(id,frame,{}, {selector_output,nullptr,nullptr,&observed})==Status::ok&&allocations==before,"cold selector clock/noalloc");};
  for(int i=0;i<4;++i){cold_send(13,capture);cold_send(16,unrelated);
   check(observed.available==std::size_t(i),"cold selector ticked on second/unrelated source");cold_send(1,generator);}
  check(observed.available==4&&observed.last.producer_epoch==generator.epoch&&rows(cold)==1,"cold selector stable compatible clock/cadence");
 }
 void unknown_clock(){auto contract=std::get<NumericContract>(pcm.view());contract.clock={"",{1,1},0};
  OwnedProcessingContract unknown;check(own_processing_contract(contract,limits,65536,unknown)==Status::ok,"selector unknown clock contract");
  auto source=node(g,1);source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs[0].contract=unknown;
  auto selector=node(g,14);for(auto* ports:{&selector.inputs,&selector.outputs})for(auto& port:*ports)port.contract=unknown;
  NumericProcessingGraph graph;graph.graph_id="selector.clockless";graph.nodes={source,selector};graph.edges={{1,14,0,0}};
  GraphExecutor executor(profile());prepare(executor,graph,pending);
  check(executor.apply(std::move(pending))==Status::ok,"selector unknown clock Apply");
  auto frame=generator.frame(256);std::get<NumericView>(frame.payload).contract=&contract;SelectorObservation observed;
  const auto before=allocations;
  check(executor.process_numeric_ports(1,frame,{}, {selector_output,nullptr,nullptr,&observed})==Status::ok&&allocations==before&&observed.unavailable==1&&observed.available==0,"disconnected selected edge unknown clock became available");
 }
 void sparse_id(){auto source=node(g,1);auto selector=node(g,14);selector.inputs.resize(1);selector.inputs[0].id=65535;
  std::get<TransformProcessingParameters>(selector.parameters).values={{"selected_input",NumericLiteral{std::uint64_t(65535)}}};
  NumericProcessingGraph graph;graph.graph_id="selector.sparse";graph.nodes={source,selector};graph.edges={{1,14,0,65535}};
  GraphExecutor executor(profile());prepare(executor,graph,pending);
  check(executor.apply(std::move(pending))==Status::ok,"sparse selector Apply");
  auto frame=generator.frame(256);SelectorObservation observed;const auto before=allocations;
  check(executor.process_numeric_ports(1,frame,{}, {selector_output,nullptr,nullptr,&observed})==Status::ok&&allocations==before&&observed.available==1,"sparse selector stable ID/no zero anchor/noalloc");
 }
};
static void input_selector(){InputSelectorFixture fixture;
 fixture.baseline();fixture.unused_port_edits();fixture.switch_active();fixture.recover_selected();fixture.disable_and_resume();fixture.grow_and_reject();
 auto contract=fixture.disconnect_selected_edge();Feed unrelated(fixture.g);unrelated.contract=contract;unrelated.epoch=200;
 fixture.disconnected_clock(unrelated);fixture.cold_disconnected_clock(unrelated);fixture.unknown_clock();fixture.sparse_id();
 std::puts("InputSelector exact PCM/active-only/retained History bits/fresh Window/truthful metadata/unavailable/noalloc PASS");
}
struct GroupObservation {std::array<unsigned,2> calls{};std::array<float,2> values{};std::array<bool,2> discontinuity{};std::array<std::size_t,2> sizes{};std::array<std::int64_t,2> ticks{};};
static void group_output(void* user,const NumericPortFrameView& frame) {
 if(frame.identity.node!=20)return;auto& o=*static_cast<GroupObservation*>(user);const auto lane=frame.identity.port;
 check(lane<2&&frame.status==Status::ok&&frame.frame.stamp.available,"group output port/status");
 check(validate_view(frame.frame,limits)==Status::ok,"group owning/borrowed storage covers declared envelope");
 const auto& view=std::get<NumericView>(frame.frame.payload);o.sizes[lane]=view.actual[0];o.ticks[lane]=frame.frame.stamp.timestamp_ticks;
 float value=0;check(load_element(view,{0,0,0},value)==Status::ok,"group value");
 ++o.calls[lane];o.values[lane]=value;o.discontinuity[lane]=frame.frame.stamp.discontinuity;
}
static NumericProcessingGraph grouped_selector_graph(NumericProcessingGraph& base){
 NumericProcessingGraph graph;graph.graph_id="grouped-selector";
 for(int id=1;id<=4;++id){auto source=node(base,1);source.id=id;graph.nodes.push_back(std::move(source));}
 const auto pcm=graph.nodes.front().outputs.front().contract;
 ConfiguredProcessingNode selector;selector.id=20;selector.kind="InputSelector";
 selector.parameters=TransformProcessingParameters{{{"selected_input",NumericLiteral{std::uint64_t{0}}},{"channels_per_stream",NumericLiteral{std::uint64_t{2}}}}};
 for(unsigned port=0;port<4;++port)selector.inputs.push_back({PortId(port),PortDirection::Input,pcm});
 auto envelope=pcm.view();std::get<NumericContract>(envelope).shape.extents[0]={0,512};OwnedProcessingContract bounded;
 check(own_processing_contract(envelope,limits,65536,bounded)==Status::ok,"group envelope");
 for(unsigned port=0;port<2;++port)selector.outputs.push_back({PortId(port),PortDirection::Output,bounded});
 graph.nodes.push_back(std::move(selector));graph.edges={{1,20,0,0},{2,20,0,1},{3,20,0,2},{4,20,0,3}};
 return graph;
}
struct GroupSelectorFixture {
 NumericProcessingGraph base=oracle_graph();NumericProcessingGraph g;GraphExecutor e{profile()};
 application::PreparedExecution pending;std::array<Feed,4> feeds;GroupObservation o;
 GroupSelectorFixture():g(make_graph()),feeds{Feed(g),Feed(g),Feed(g),Feed(g)}{apply("group initial Apply");}
 NumericProcessingGraph make_graph(){geometry(base,48000,4096,317);return grouped_selector_graph(base);}
 void apply(const char* reason){prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,reason);}
 void send(unsigned id,std::size_t count=256){auto& feed=feeds[id-1];feed.epoch=100+id;
  auto frame=feed.frame(count);std::fill(feed.samples.begin(),feed.samples.end(),float(id));
  const auto before=allocations;
  check(e.process_numeric_ports(id,frame,{}, {group_output,nullptr,nullptr,&o})==Status::ok&&allocations==before,"group synchronous/noalloc");
 }
 void baseline(){for(int tick=0;tick<3;++tick){send(1);send(2);send(3);send(4);}
  check(o.calls==std::array<unsigned,2>{3,3}&&o.values==std::array<float,2>{1,2}&&!o.discontinuity[0]&&!o.discontinuity[1],"group active lanes only / independent continuity");}
 void switch_lanes(){std::get<TransformProcessingParameters>(node(g,20).parameters).values[0].value=NumericLiteral{std::uint64_t{1}};
  apply("group switch Apply");send(1);send(2);check(o.calls==std::array<unsigned,2>{3,3},"old stream still routed");
  send(3);send(4);check(o.values==std::array<float,2>{3,4}&&o.discontinuity[0]&&o.discontinuity[1],"all lanes switch and mark boundary");
  send(3);send(4);check(!o.discontinuity[0]&&!o.discontinuity[1],"group boundary persisted across frames");}
 void missing_lane(){std::erase_if(g.edges,[](const auto& edge){return edge.from==4;});apply("group missing lane Apply");
  const auto calls=o.calls;send(3,317);
  check(o.calls[0]==calls[0]+1&&o.calls[1]==calls[1]+1&&o.values[0]==3&&o.values[1]==0,"missing channel typed zero, no mono duplication");
  check(o.sizes[0]==317&&o.sizes[1]==317&&o.ticks[0]==o.ticks[1],"missing channel chunk size and timestamp differ");
  send(1);send(2);send(4);check(o.calls[0]==calls[0]+1&&o.calls[1]==calls[1]+1,"missing lane double tick");}
 void configure_orphan(){auto orphan=node(base,1);orphan.id=30;orphan.kind="PcmResampler";
  const auto pcm=g.nodes.front().outputs.front().contract;orphan.inputs={{0,PortDirection::Input,pcm}};
  orphan.parameters=TransformProcessingParameters{{{"input_rate",NumericLiteral{std::uint64_t{48000}}},{"output_rate",NumericLiteral{std::uint64_t{48000}}}}};
  auto resampled=pcm.view();std::get<NumericContract>(resampled).shape.extents[0]={1,318};OwnedProcessingContract owner;
  check(own_processing_contract(resampled,limits,65536,owner)==Status::ok,"orphan resampler output");
  orphan.outputs={{0,PortDirection::Output,owner}};node(g,20).inputs[3].contract=owner;
  g.nodes.push_back(std::move(orphan));g.edges.push_back({30,20,0,3});
 }
 void orphan_resampler(){configure_orphan();GraphExecutor cold(profile());prepare(cold,g,pending);
  check(cold.apply(std::move(pending))==Status::ok,"cold missing physical lane Apply");
  auto frame=feeds[2].frame(123);std::fill(feeds[2].samples.begin(),feeds[2].samples.end(),3.f);
  const auto before=allocations;
  check(cold.process_numeric_ports(3,frame,{}, {group_output,nullptr,nullptr,&o})==Status::ok&&allocations==before,"cold orphan noalloc");
  check(o.values[1]==0&&o.sizes[0]==123&&o.sizes[1]==123&&o.ticks[0]==o.ticks[1],"cold orphan channel did not follow selected chunk");
 }
};
static void grouped_selector(){GroupSelectorFixture fixture;
 fixture.baseline();fixture.switch_lanes();fixture.missing_lane();fixture.orphan_resampler();
 std::puts("Grouped selector: two streams/two mono lanes/atomic selection/continuity/missing silence/noalloc PASS");
}
struct AlignedObserved {
 std::array<float,8> values{};std::array<float,317> samples{};std::array<unsigned,8> calls{};std::size_t count=0;FrameStamp stamp{};unsigned unavailable=0;
};
static void observe_aligned_samples(AlignedObserved& out,const NumericView& value){
 if(value.contract->shape.rank!=Rank::Vector)return;
 for(std::size_t i=0;i<value.actual[0];++i){check(i<out.samples.size(),"aligned bounded observation");
  check(load_element(value,{0,i,0},out.samples[i])==Status::ok,"aligned full payload observation");}
}
static void aligned_output(void* user,const NumericPortFrameView& frame){if(frame.identity.node!=20)return;auto& out=*static_cast<AlignedObserved*>(user);
 if(!frame.frame.stamp.available){++out.unavailable;return;}check(validate_view(frame.frame,limits)==Status::ok,"aligned output valid");
 const auto& value=std::get<NumericView>(frame.frame.payload);const auto p=frame.identity.port;check(p<out.values.size(),"aligned output port");
 check(load_element(value,{0,0,0},out.values[p])==Status::ok,"aligned output value");
 observe_aligned_samples(out,value);
 ++out.calls[p];out.count=value.actual[0];out.stamp=frame.frame.stamp;out.stamp.stream_id={};}
static ConfiguredProcessingNode aligned_source(NumericProcessingGraph& base,std::size_t lanes){
 auto source=node(base,1);source.kind="AudioSource";
 source.parameters=EndpointProcessingParameters{};auto pcm=source.outputs.front().contract;source.outputs.clear();
 for(std::size_t lane=0;lane<lanes;++lane)source.outputs.push_back({PortId(lane),PortDirection::Output,pcm});
 return source;
}
static void aligned_comparison_output(ConfiguredProcessingNode& target,const OwnedProcessingContract& pcm){
 target.parameters=TransformProcessingParameters{{{"window_samples",NumericLiteral{std::uint64_t{512}}},{"silence_threshold",NumericLiteral{1e-12}}}};
 auto contract=std::get<NumericContract>(pcm.view());contract.semantic=Semantic::Scalar;contract.shape.rank=Rank::Scalar;
 contract.shape.axes={Axis::None,Axis::None};contract.shape.extents={ExtentBounds{1,1},ExtentBounds{1,1}};contract.coordinates={};
 OwnedProcessingContract scalar;
 check(own_processing_contract(contract,limits,65536,scalar)==Status::ok,"comparison scalar own");
 target.outputs={{0,PortDirection::Output,scalar}};
}
static void aligned_mixer_output(ConfiguredProcessingNode& target,const OwnedProcessingContract& pcm,std::size_t lanes){
 MixerProcessingParameters params;params.mode=MixerMode::WeightedMean;params.consumption=MixerConsumption::SourceAligned;
 params.weights.clear();
 for(std::size_t lane=0;lane<lanes;++lane)params.weights.push_back({PortId(lane),1});
 target.parameters=std::move(params);target.outputs={{0,PortDirection::Output,pcm}};
}
static NumericProcessingGraph aligned_fixture(std::size_t lanes,bool comparison=false,bool routing=false){
 auto base=oracle_graph();geometry(base,48000,4096,317);
 NumericProcessingGraph graph;graph.graph_id="aligned.regression";graph.nodes.push_back(aligned_source(base,lanes));
 const auto pcm=graph.nodes.front().outputs.front().contract;
 ConfiguredProcessingNode target;target.id=20;target.kind=routing?"Switch":comparison?"ChannelComparison":"Mixer";
 for(std::size_t lane=0;lane<lanes;++lane){target.inputs.push_back({PortId(lane),PortDirection::Input,pcm});
  graph.edges.push_back({1,20,PortId(lane),PortId(lane)});}
 if(routing){target.parameters=RouteProcessingParameters{3,1,true,2};
  for(PortId port=0;port<6;++port)target.outputs.push_back({port,PortDirection::Output,pcm});}
 else if(comparison)aligned_comparison_output(target,pcm);
 else aligned_mixer_output(target,pcm,lanes);
 graph.nodes.push_back(std::move(target));return graph;
}
static NumericRuntimeProfile aligned_profile(){auto value=profile();value.admission.max_ports_per_node=16;
 // Ten independent 100ms provenance FIFOs require an explicit device budget.
 value.resources.maximum.payload_bytes=4*1048576;value.resources.max_peak_bytes=16*1048576;return value;}
struct AlignedRunner {
 NumericRuntimeProfile p=aligned_profile();NumericProcessingGraph graph=aligned_fixture(10);
 GraphExecutor executor{p};Feed feed{graph};AlignedObserved out;
 AlignedRunner(){activate(executor,graph);}
 void activate(GraphExecutor& target,const NumericProcessingGraph& fixture){application::PreparedExecution pending;
  const auto status=target.prepare_numeric(fixture,{p.admission,p.resources},pending);
  if(status!=Status::ok){std::fprintf(stderr,"aligned prepare status=%d nodes=%zu ports=%zu payload_budget=%zu peak_budget=%zu\n",int(status),fixture.nodes.size(),fixture.nodes.front().outputs.size(),p.resources.maximum.payload_bytes,p.resources.max_peak_bytes);
   std::vector<std::size_t> workspace(fixture.nodes.size()*2);ProcessingGraphAdmission admission;
   const auto diagnostic=admit_processing_graph(fixture,p.admission,PreparedGraph::numeric_capabilities(p),workspace,admission);
   std::fprintf(stderr,"aligned admission error=%d node=%d port=%u payload=%zu peak=%zu\n",int(diagnostic.error),int(diagnostic.node),unsigned(diagnostic.port),admission.resources.payload_bytes,admission.peak_bytes);}
  check(status==Status::ok&&target.apply(std::move(pending))==Status::ok,"aligned Apply");
 }
 void send(GraphExecutor& target,PortId lane,std::int64_t begin,std::size_t count,float value,bool discontinuity=false,Ratio latency={0,1}){
  auto frame=feed.frame(count);std::fill(feed.samples.begin(),feed.samples.end(),value);
  frame.stamp.sample_begin=begin;frame.stamp.sample_count=count;frame.stamp.has_sample_interval=true;
  frame.stamp.timestamp_ticks=begin+std::int64_t(count)-1;frame.stamp.discontinuity=discontinuity;frame.stamp.latency_seconds=latency;
  const auto before=allocations;const auto status=target.process_numeric_ports(1,lane,frame,{}, {aligned_output,nullptr,nullptr,&out});
  check(status==Status::ok&&allocations==before,"aligned processing noalloc");
 }
 void ten_inputs(){for(PortId lane=0;lane<9;++lane)send(executor,lane,0,127,float(lane+1));
  check(out.calls[0]==0,"aligned cannot emit partial inputs");send(executor,9,0,127,10);
  check(out.calls[0]==1&&out.count==127&&out.values[0]==5.5f&&out.stamp.sample_begin==0,"ten-input aligned mean");
  for(PortId lane=0;lane<10;++lane)send(executor,lane,127,193,float(lane+1));
  check(out.calls[0]==2&&out.stamp.sample_begin==127&&!out.stamp.discontinuity,"irregular aligned continuity");
  send(executor,0,447,127,1);
  for(PortId lane=1;lane<10;++lane)send(executor,lane,320,254,float(lane+1));
  check(out.calls[0]==3&&out.stamp.sample_begin==447&&out.stamp.discontinuity,"asymmetric gap matches positions");
 }
 void integer_latency(){auto graph=aligned_fixture(2);GraphExecutor target(p);activate(target,graph);out={};
  send(target,0,0,123,2);send(target,1,10,123,4,false,{10,48000});
  check(out.calls[0]==1&&out.values[0]==3&&out.stamp.sample_begin==10&&out.stamp.latency_samples==10,"integer latency aligns content positions");
 }
 void fractional_latency_change(){auto graph=aligned_fixture(2);GraphExecutor target(p);activate(target,graph);out={};
  send(target,0,0,4,1);send(target,0,4,4,8,false,{1,96000});send(target,1,0,8,2);
  check(out.calls[0]==1&&out.count==4&&out.values[0]==1.5f&&out.stamp.sample_begin==0,"fractional latency change preserves only aligned prefix");
  const auto stats=target.mixer_input_stats(20,0);check(stats&&stats->dropped_samples>=4,"fractional unmatched tail discarded");
  send(target,0,8,4,4);send(target,1,8,4,2);
  check(out.calls[0]==2&&out.count==4&&out.values[0]==3&&out.stamp.sample_begin==8&&out.stamp.discontinuity,"fractional tail reset recovers without stale data");
 }
 void integer_latency_change(){auto graph=aligned_fixture(2);GraphExecutor target(p);activate(target,graph);out={};
  send(target,0,0,4,1);send(target,0,4,4,8,false,{1,48000});send(target,1,0,8,2);
  check(out.calls[0]==2&&out.count==3&&out.values[0]==5&&out.stamp.sample_begin==5&&out.stamp.discontinuity,"integer latency change splits prefix and matches shifted tail");
 }
 void weighted_single_and_cancellation(){auto one=aligned_fixture(1);
  auto& params=std::get<MixerProcessingParameters>(node(one,20).parameters);params.mode=MixerMode::WeightedSum;params.weights[0].weight=3;
  GraphExecutor single(p);activate(single,one);out={};send(single,0,0,123,2);
  check(out.calls[0]==1&&out.values[0]==6,"N1 weighted sum has no hidden clipping");
  auto anti=aligned_fixture(2);GraphExecutor cancellation(p);activate(cancellation,anti);out={};
  send(cancellation,0,0,123,2);send(cancellation,1,0,123,-2);
  check(out.values[0]==0,"aligned antiphase cancellation");
 }
 void bounded_overflow(){auto graph=aligned_fixture(2);GraphExecutor target(p);activate(target,graph);out={};
  for(std::int64_t begin=0;begin<10240;begin+=256)send(target,0,begin,256,2);
  send(target,1,9600,256,4);const auto stats=target.mixer_input_stats(20,0);
  check(out.calls[0]==1&&out.stamp.sample_begin==9600&&out.stamp.discontinuity&&stats&&stats->dropped_samples>0&&stats->capacity==4800,"aligned overflow bounded/resynchronizes by sample position");
 }
 void comparison_levels(GraphExecutor& target){
  for(std::int64_t begin=0;begin<512;begin+=256){send(target,0,begin,256,1);send(target,1,begin,256,1);}
  check(out.calls[0]==1&&out.values[0]==0&&out.stamp.sample_count==512,"equal energy balance");
  for(std::int64_t begin=512;begin<1024;begin+=256){send(target,0,begin,256,0);send(target,1,begin,256,1);}
  check(out.calls[0]==2&&out.values[0]==1,"only channel2 balance");
  for(std::int64_t begin=1024;begin<1536;begin+=256){send(target,0,begin,256,1);send(target,1,begin,256,0);}
  check(out.values[0]==-1,"only channel1 balance");
  for(std::int64_t begin=1536;begin<2048;begin+=256){send(target,0,begin,256,0);send(target,1,begin,256,0);}
  check(out.values[0]==0,"silent balance");
 }
 void comparison_boundary(){auto graph=aligned_fixture(2,true);GraphExecutor target(p);activate(target,graph);out={};
  comparison_levels(target);send(target,0,2304,256,1,true);
  check(out.unavailable>0,"comparison gap clears displayed metric");
  const auto prior=out.calls[0];send(target,0,0,256,1,true);send(target,1,0,256,1,true);
  send(target,0,256,256,1);send(target,1,256,256,1);
  check(out.calls[0]==prior+1&&out.values[0]==0&&out.stamp.sample_begin==0&&out.stamp.discontinuity,"paired discontinuity preserves new first interval");
 }
 void grouped_switch(){auto graph=aligned_fixture(2,false,true);GraphExecutor target(p);activate(target,graph);out={};
  send(target,0,0,123,3);send(target,1,0,123,7);
  check(out.calls==std::array<unsigned,8>{1,1,1,1,1,1,0,0}&&out.values==std::array<float,8>{0,0,3,7,0,0,0,0},"grouped output switch maps lane1-to1 lane2-to2");
 }
};
static void aligned_regressions(){AlignedRunner runner;
 runner.ten_inputs();runner.integer_latency();runner.fractional_latency_change();runner.integer_latency_change();
 runner.weighted_single_and_cancellation();runner.bounded_overflow();runner.comparison_boundary();runner.grouped_switch();
 std::puts("SourceAligned N10/irregular/gap, comparison balance/silence/invalidation, grouped Switch/noalloc PASS");
}
static NumericRuntimeProfile startup_profile(){
 // Match the desktop engine's actual admission/kernel/reservation profile.
 constexpr std::size_t fixed=256*(262144+16384)+(384+48)*1024*1024;
 constexpr FrameLimits desktopLimits{8,131072,524288,16384,256,8192};
 NumericRuntimeProfile p{{512,2048,16,32,1024*1024,4*1024*1024,desktopLimits},
  {{16*1024*1024,32*1024*1024,16*1024*1024,1024*1024},fixed,576*1024*1024},
  {32768,8,262144,262144,16*1024*1024},{true,true,true},true};
 p.music_enabled=true;p.experimental_resampling_enabled=true;
#ifdef BARELIEF_TEST_DESKTOP_SPEEX
 p.resampler_backend=&desktop_resampling::speex_backend_factory();
#endif
 return p;
}
static void add_unique_ancestor(std::vector<NodeId>& keep,NodeId id){
 if(std::find(keep.begin(),keep.end(),id)==keep.end())keep.push_back(id);
}
static NumericProcessingGraph startup_fragment(const NumericProcessingGraph& graph,NodeId target){
 auto fragment=graph;std::vector<NodeId> keep{target};
 for(size_t i=0;i<keep.size();++i){
  for(const auto& edge:graph.edges)if(edge.to==keep[i])add_unique_ancestor(keep,edge.from);
  for(const auto& binding:graph.configuration_bindings)if(binding.to==keep[i])add_unique_ancestor(keep,binding.from);
 }
 const auto retained=[&](NodeId id){return std::find(keep.begin(),keep.end(),id)!=keep.end();};
 std::erase_if(fragment.nodes,[&](const auto& n){return !retained(n.id);});
 std::erase_if(fragment.edges,[&](const auto& e){return !retained(e.from)||!retained(e.to);});
 std::erase_if(fragment.configuration_bindings,[&](const auto& b){return !retained(b.from)||!retained(b.to);});
 return fragment;
}
static void diagnose_startup_failure(const NumericProcessingGraph& graph,const NumericRuntimeProfile& profile,Status status){
 std::fprintf(stderr,"startup prepare status=%d nodes=%zu\n",int(status),graph.nodes.size());
 NumericProcessingGraph evaluated,executable;
 const auto diagnostic=resolve_numeric_parameter_bindings(graph,profile.admission,evaluated,executable);
 std::fprintf(stderr,"startup bindings error=%u node=%lld port=%u\n",unsigned(diagnostic.error),static_cast<long long>(diagnostic.node),unsigned(diagnostic.port));
 // Complete data/configuration ancestry identifies the failing prepared fragment.
 for(const auto& target:graph.nodes){auto fragment=startup_fragment(graph,target.id);
  std::shared_ptr<const PreparedGraph> part;std::unique_ptr<RuntimeContext> state;
  const auto failed=PreparedGraph::prepare_numeric(fragment,profile,part,state);
  if(failed!=Status::ok)std::fprintf(stderr,"startup ancestor target=%d kind=%s status=%d nodes=%zu\n",int(target.id),target.kind.c_str(),int(failed),fragment.nodes.size());
 }
}
static void mono_source_discovery(NumericProcessingGraph& graph,NodeId source_id){
 // Native mono discovery removes extra PCM lanes, preserving the explicit Fs
 // scalar and its wires. Output count includes metadata ports, not just PCM.
 auto& physical=node(graph,source_id);
 std::erase_if(physical.outputs,[](const auto& port){return port.id!=0 && port.id!=sample_rate_output;});
 check(data_outputs(physical).size()==1 && std::any_of(physical.outputs.begin(),physical.outputs.end(),[](const auto& port){return port.id==sample_rate_output;}),"mono discovery preserves PCM and explicit Fs");
 std::erase_if(graph.edges,[&](const auto& edge){return edge.from==source_id && edge.output_port!=0 && edge.output_port!=sample_rate_output;});
}
static void mono_selector_variants(NumericProcessingGraph& graph,const NumericRuntimeProfile& profile){
 for(std::uint64_t selected=0;selected<2;++selected){
  auto& selector=std::get<TransformProcessingParameters>(node(graph,200).parameters);
  for(auto& field:selector.values)if(field.key=="selected_input")field.value=NumericLiteral{selected};
  std::shared_ptr<const PreparedGraph> mono_plan;std::unique_ptr<RuntimeContext> mono_state;
  const auto mono_status=PreparedGraph::prepare_numeric(graph,profile,mono_plan,mono_state);
  if(mono_status!=Status::ok)std::fprintf(stderr,"startup mono48000 selected=%llu status=%d\n",(unsigned long long)selected,int(mono_status));
  check(mono_status==Status::ok,"startup mono discovery preserves explicit bindings and disconnected physical lanes");
 }
}
static void startup_preparation_oracle(){auto graph=numeric_startup_graph();const auto profile=startup_profile();
 std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
 const auto status=PreparedGraph::prepare_numeric(graph,profile,plan,context);
 if(status!=Status::ok)diagnose_startup_failure(graph,profile,status);
 check(status==Status::ok,"startup complete preparation");
 mono_source_discovery(graph,13);
 mono_selector_variants(graph,profile);
}
#include "aligned_numerical_acceptance.h"
static void moved_from_context_rejected(){
 auto graph=oracle_graph();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
 const auto prepared=PreparedGraph::prepare_numeric(graph,profile(),plan,context);
  if(prepared!=Status::ok)std::fprintf(stderr,"moved context prepare status=%d\n",int(prepared));
  check(prepared==Status::ok,"moved context prepare");
 Feed feed(graph);auto input=feed.frame(256);RuntimeContext moved(std::move(*context));
 const auto before=allocations;
 check(plan->process_numeric_ports(1,input,*context,{})==Status::invalid_argument,"moved-from context rejected before dereference");
 check(plan->process_numeric_ports(1,0,input,*context,{})==Status::invalid_argument,"explicit port rejects moved-from context");
 check(allocations==before,"moved-from context rejection allocates");
 check(plan->process_numeric_ports(1,input,moved,{})==Status::ok,"moved context retains prepared ownership");
}
int main(){moved_from_context_rejected();startup_preparation_oracle();check(numeric_test_graph().execution_interval==Ratio(1,120),"new graph default remains120Hz");aligned_regressions();aligned_acceptance::resampler_range();aligned_acceptance::impulse_and_generation();aligned_acceptance::group_revision();aligned_acceptance::deadline_regressions();aligned_acceptance::physical_grid_matrix();aligned_acceptance::stereo_windows_and_mix();core_regression::run();independent_windows();latest_state_at_apply();selective_reset();source_fences();ring_capacity_and_restart();repeated_dsp_restart();input_selector();grouped_selector();}
