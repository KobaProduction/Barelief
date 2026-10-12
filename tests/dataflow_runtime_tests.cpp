#include "canonical_runtime_test_fixture.h"
#include "canonical_core_regressions.h"
#include <bit>
using namespace canonical_test;
static std::array<std::byte,50*8> mel_coordinates(){
 std::array<std::byte,50*8> bytes{};
 for(std::size_t band=0;band<50;++band){const auto bits=std::bit_cast<std::uint64_t>(double(band+1)*100.);for(unsigned byte=0;byte<8;++byte)bytes[band*8+byte]=std::byte((bits>>(byte*8))&255);}
 return bytes;
}
struct Spectra{std::array<std::size_t,2> count{};std::array<std::int64_t,2> end{};std::array<std::size_t,2> peak{};};
static void spectrum(void* user,const NumericPortFrameView& f){if(f.status!=Status::ok || (f.identity.node!=4&&f.identity.node!=24))return;auto& result=*static_cast<Spectra*>(user);const auto& view=std::get<NumericView>(f.frame.payload);check(view.planes.size()==1,"mono FFT output");const auto slot=f.identity.node==4?0:1;std::size_t peak=0;float maximum=-1;
 for(std::size_t i=0;i<view.actual[0];++i){float value=0;check(load_element(view,{0,i,0},value)==Status::ok&&std::isfinite(value),"FFT finite");if(value>maximum){maximum=value;peak=i;}}
 result.peak[slot]=peak;++result.count[slot];result.end[slot]=f.frame.stamp.timestamp_ticks;}
static void independent_windows(){auto g=oracle_graph();std::erase_if(g.nodes,[](const auto& n){return n.id>4;});g.configuration_bindings.clear();g.edges={{1,2,0,0},{2,3,0,0},{3,4,0,0}};geometry(g,8192,2048,256);
 auto short_graph=oracle_graph();std::erase_if(short_graph.nodes,[](const auto& n){return n.id>4;});short_graph.configuration_bindings.clear();geometry(short_graph,8192,512,256);
 for(auto n:short_graph.nodes){if(n.id!=1){n.id+=20;g.nodes.push_back(std::move(n));}}g.edges.push_back({1,22,0,0});g.edges.push_back({22,23,0,0});g.edges.push_back({23,24,0,0});
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"apply dual windows");Feed feed(g);Spectra result;
 for(int i=0;i<32;++i)feed.process(e,256,{spectrum,nullptr,nullptr,&result});for(std::size_t i=0;i<2;++i)check(result.count[i]==60&&result.end[i]==8192&&result.peak[i]==(i==0?128:32),"independent mono window schedule/timestamp");std::puts("independent windows2048/512 mono peaks128/32 PASS");}
struct RetainedHistory {std::size_t filled=0,bands=0;std::uint64_t sequence=0;bool discontinuity=false;std::vector<std::uint64_t> bits;};
static RetainedHistory retained_history(const GraphExecutor& e,NodeId id=7){DataFrameView frame;std::uint64_t filled=0;check(e.plan()->history(id,0,*e.context(),frame,&filled)==Status::ok,"retain History observation");const auto& view=std::get<NumericView>(frame.payload);RetainedHistory result;result.sequence=frame.stamp.sequence;result.discontinuity=frame.stamp.discontinuity;result.filled=std::size_t(filled);result.bands=view.actual[1];result.bits.resize(result.filled*result.bands);
 for(std::size_t r=0;r<result.filled;++r)for(std::size_t b=0;b<result.bands;++b)check(load_bits(view,{0,view.actual[0]-result.filled+r,b},view.contract->dtype,result.bits[r*result.bands+b])==Status::ok,"retain History exact bits");return result;}
static void appended_history(const RetainedHistory& before,const GraphExecutor& e,NodeId id=7){const auto after=retained_history(e,id);check(after.filled==before.filled+1&&after.bands==before.bands,"compatible source boundary appends one History row");check(std::equal(before.bits.begin(),before.bits.end(),after.bits.begin()),"compatible source boundary preserves all old History bits");if(!after.discontinuity || after.sequence!=before.sequence+1)std::fprintf(stderr,"History boundary before=%llu after=%llu discontinuity=%d\n",(unsigned long long)before.sequence,(unsigned long long)after.sequence,int(after.discontinuity));check(after.discontinuity&&after.sequence==before.sequence+1,"compatible source boundary marks discontinuity without restarting History sequence");}
static void latest_state_at_apply(){auto g=oracle_graph();GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"initial apply");Feed feed(g);for(int i=0;i<32;++i)feed.process(e,256);
 auto changed=g;node(changed,9).enabled=false;prepare(e,changed,pending);for(int i=0;i<8;++i)feed.process(e,256);const auto before=rows(e);check(e.apply(std::move(pending))==Status::ok&&rows(e)==before,"Apply copied stale Prepare history");
 prepare(e,g,pending);check(e.shutdown()==Status::ok,"shutdown pending");check(e.apply(std::move(pending))==Status::invalid_argument&&!e.running(),"shutdown fences pending Apply");check(restart(e,g)==Status::ok,"re-admit after shutdown");for(int i=0;i<4;++i){feed.process(e,256);}check(rows(e)==1,"shutdown resurrected old History");
 prepare(e,g,pending);GraphExecutor foreign(profile());check(foreign.apply(std::move(pending))==Status::invalid_argument&&bool(pending),"foreign executor consumes bundle");check(e.apply(std::move(pending))==Status::ok,"owner Apply after foreign reject");
 for(int i=0;i<8;++i){feed.process(e,256);}check(rows(e)>1,"restart accumulation");const auto before_epoch=retained_history(e);++feed.epoch;for(int i=0;i<4;++i){feed.process(e,256);}appended_history(before_epoch,e);std::puts("late Apply/latest state, shutdown fence, foreign bundle, compatible producer History retention PASS");}
static void selective_reset(){auto base=oracle_graph();auto g=base;
 for(auto n:base.nodes){n.id+=20;g.nodes.push_back(std::move(n));}for(auto edge:base.edges){edge.from+=20;edge.to+=20;g.edges.push_back(edge);}
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"two branches Apply");Feed left(base),right(base);
 auto process_right=[&](){auto f=right.frame(256);auto before=allocations;check(e.process_numeric_ports(21,f,{}, {})==Status::ok&&allocations==before,"right branch process/noalloc");};
 for(int i=0;i<32;++i){left.process(e,256);process_right();}const auto original=rows(e,7);check(rows(e,27)==original,"two branches counts");
 const auto before_epoch=retained_history(e,27);++right.epoch;for(int i=0;i<4;++i){process_right();}appended_history(before_epoch,e,27);check(rows(e,7)==original,"source restart reset independent branch");
 for(auto& edge:g.edges){if(edge.to==7)edge.from=26;}const auto untouched=rows(e,27);prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok&&rows(e,27)==untouched,"clock edit reset independent History");
 for(int i=0;i<4;++i){process_right();}check(rows(e,7)==1&&rows(e,27)==untouched+1,"source identity change failed selective reset");
 auto& history=node(g,7);std::get<HistoryProcessingParameters>(history.parameters).capacity=200;
 for(auto* ports:{&history.outputs,&node(g,8).inputs})for(auto& port:*ports){auto c=std::get<NumericContract>(port.contract.view());if(c.semantic!=Semantic::History)continue;c.shape.extents[0]={0,200};check(own_processing_contract(c,limits,65536,port.contract)==Status::ok,"resize History contract");}
 const auto independent=rows(e,27),retained_count=rows(e,7);DataFrameView before_resize;
 check(e.plan()->history(7,0,*e.context(),before_resize)==Status::ok,"resize newest baseline");const auto& before_view=std::get<NumericView>(before_resize.payload);std::array<std::uint64_t,50> retained_bits{};
 for(std::size_t b=0;b<50;++b)check(load_bits(before_view,{0,before_view.actual[0]-1,b},DType::Float32,retained_bits[b])==Status::ok,"resize baseline bits");
 prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok&&rows(e,27)==independent&&rows(e,7)==retained_count,"capacity Apply changed retained occupancy or independent branch");
 DataFrameView after_resize;check(e.plan()->history(7,0,*e.context(),after_resize)==Status::ok,"resize retained snapshot");const auto& after_view=std::get<NumericView>(after_resize.payload);
 for(std::size_t b=0;b<50;++b){std::uint64_t actual=0;check(load_bits(after_view,{0,after_view.actual[0]-1,b},DType::Float32,actual)==Status::ok&&actual==retained_bits[b],"capacity Apply changed freshest bits");}
 for(int i=0;i<4;++i){process_right();}check(rows(e,7)==retained_count+1&&rows(e,27)==independent+1,"capacity edit reset accumulation");std::puts("source restart/rebind selective reset and capacity preservation PASS");}
static void source_fences(){auto g=oracle_graph();auto input=node(g,6).outputs[0].contract;
 auto& source=node(g,1);source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs[0].contract=input;
 std::erase_if(g.nodes,[](const auto& n){return n.id!=1&&n.id!=7;});g.configuration_bindings.clear();g.edges={{1,7,0,0}};GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"marker Apply");Feed feed(g);const auto frequencyBytes=mel_coordinates();
 auto send=[&](std::uint64_t epoch,std::uint64_t gen,std::uint64_t seq,bool discontinuity=false,bool available=true){auto f=feed.frame(50);std::get<NumericView>(f.payload).planes=std::span(feed.planes).first(1);std::get<NumericView>(f.payload).coordinates.axes[0]=frequencyBytes;f.stamp.producer_epoch=epoch;f.stamp.generation=gen;f.stamp.sequence=seq;f.stamp.discontinuity=discontinuity;f.stamp.available=available;if(!available)std::get<NumericView>(f.payload).storage={};const auto before=allocations;auto status=e.process_numeric_ports(1,f,{});check(allocations==before,"marker hot allocation");return status;};
 check(send(17,0,100)==Status::ok&&send(17,0,101)==Status::ok&&rows(e)==2,"generation zero baseline");
 auto before=retained_history(e);check(send(17,1,1)==Status::ok,"generation transition sequence restart");appended_history(before,e);check(send(17,0,102)==Status::would_block&&rows(e)==3,"old generation accepted");
 before=retained_history(e);check(send(99,1,1)==Status::ok,"new producer sequence restart");appended_history(before,e);check(send(17,1,103)==Status::would_block&&rows(e)==4,"retired producer accepted");
 before=retained_history(e);check(send(99,1,2,true)==Status::ok,"explicit discontinuity");appended_history(before,e);check(send(99,1,2,true)==Status::would_block&&rows(e)==5,"duplicate discontinuity mutates History");
 before=retained_history(e);check(send(99,2,0,false,false)==Status::ok,"unavailable generation marker");check(send(99,1,3)==Status::would_block,"old data revived marker");check(send(99,2,3)==Status::ok,"current generation recovery");appended_history(before,e);std::puts("generation0/restart/retired producer/duplicate discontinuity/unavailable fences PASS");}
// Retained ring and restart predicates from the old mixed runtime_tests fixture.
// Capacity edits preserve newest real rows and extend only the older typed-zero tail.
static void ring_capacity_and_restart(){
 auto g=oracle_graph();auto contract=node(g,6).outputs[0].contract;
 auto& source=node(g,1);source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs[0].contract=contract;
 std::erase_if(g.nodes,[](const auto& n){return n.id!=1&&n.id!=7;});g.configuration_bindings.clear();g.edges={{1,7,0,0}};
 auto capacity=[&](std::size_t count){auto& h=node(g,7);std::get<HistoryProcessingParameters>(h.parameters).capacity=count;auto c=std::get<NumericContract>(h.outputs[0].contract.view());c.shape.extents[0]={0,count};check(own_processing_contract(c,limits,65536,h.outputs[0].contract)==Status::ok,"ring capacity contract");};
 capacity(400);GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"ring Apply");
 auto c=std::get<NumericContract>(node(g,1).outputs[0].contract.view());std::array<float,50> values{};const PlaneLayout plane{0,{4,0}};std::uint64_t sequence=0,expected_ordinal=0;
 const auto frequencyBytes=mel_coordinates();
 auto input=[&](){values.fill(float(++sequence));FrameStamp stamp;stamp.sequence=sequence;stamp.source_sequence=sequence;stamp.producer_epoch=21;stamp.generation=3;stamp.timestamp_ticks=std::int64_t(sequence);return DataFrameView{stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{50,1},{},CoordinateView{{std::span<const std::byte>(frequencyBytes),{}}}}};};
 auto feed=[&](){++expected_ordinal;auto frame=input();const auto before=allocations;check(e.process_numeric_ports(1,frame,{})==Status::ok&&allocations==before,"ring process noalloc");};
 auto exact=[&](std::initializer_list<float> expected){DataFrameView frame;std::uint64_t filled=0,ordinal=0;const auto before=allocations;check(e.plan()->history(7,0,*e.context(),frame,&filled,&ordinal)==Status::ok&&allocations==before,"ring query noalloc");auto view=std::get<NumericView>(frame.payload);check(validate_view(frame,limits)==Status::ok && view.actual[0]==std::get<HistoryProcessingParameters>(node(g,7).parameters).capacity&&filled==expected.size()&&view.actual[1]==50,"fixed ring dimensions/real occupancy");
  check(ordinal==expected_ordinal,"resize preserves accepted ordinal");
  for(std::size_t row=0;row<view.actual[0]-filled;++row)for(std::size_t band=0;band<50;++band){float value=1;check(load_element(view,{0,row,band},value)==Status::ok&&value==0,"fixed zero old tail");}
  check(view.contract->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && view.contract->coordinates[0].unit==NumericUnit::Seconds && view.coordinates.axes[0].size()==view.actual[0]*8,"ring explicit source-clock times");
  for(std::size_t byte=0;byte<(view.actual[0]-filled)*8;++byte)check(view.coordinates.axes[0][byte]==std::byte{0},"zero older padding must not fabricate observation times");
  std::size_t row=view.actual[0]-filled;for(float want:expected){for(std::size_t band=0;band<50;++band){float actual=0;check(load_element(view,{0,row,band},actual)==Status::ok&&actual==want,"ring logical order/write position");}
   std::uint64_t bits=0;for(unsigned byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned>(view.coordinates.axes[0][row*8+byte]))<<(8*byte);
   check(std::abs(std::bit_cast<double>(bits)-double(want)/48000.)<1e-15,"ring times survive wrap/compatible Apply/reset exactly with values");++row;}};
 exact({});capacity(4);prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"empty History resize Apply");exact({});
 capacity(8);prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"empty History expansion Apply");exact({});
 capacity(4);prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"empty History shrink Apply");exact({});
 for(int i=0;i<7;++i){feed();}exact({4,5,6,7});prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"full ring compatible Apply");exact({4,5,6,7});feed();exact({5,6,7,8});
 capacity(8);prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"grow compatible Apply");exact({5,6,7,8});feed();exact({5,6,7,8,9});feed();exact({5,6,7,8,9,10});
 capacity(2);prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"shrink compatible Apply");exact({9,10});feed();exact({10,11});feed();exact({11,12});feed();exact({12,13});
 for(int cycle=0;cycle<10;++cycle){check(e.shutdown()==Status::ok&&e.shutdown()==Status::ok&&!e.running(),"repeated owner shutdown");auto frame=input();check(e.process_numeric_ports(1,frame,{})==Status::would_block,"shutdown graph delivered");check(restart(e,g)==Status::ok&&restart(e,g)==Status::ok,"repeated re-admission");expected_ordinal=0;feed();exact({float(sequence)});feed();exact({float(sequence-1),float(sequence)});}
 std::puts("mixed-runtime ring wrap/compatible Apply/explicit grow-shrink reset/write position/10 restart cycles PASS");
}
static void repeated_dsp_restart(){auto g=oracle_graph();GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"DSP restart Apply");Feed feed(g);
 for(int cycle=0;cycle<10;++cycle){check(e.shutdown()==Status::ok&&e.shutdown()==Status::ok,"DSP repeated owner shutdown");auto blocked=feed.frame(256);check(e.process_numeric_ports(1,blocked,{})==Status::would_block,"shutdown DSP accepts input");check(restart(e,g)==Status::ok&&restart(e,g)==Status::ok,"DSP repeated re-admission");for(int i=0;i<4;++i){feed.process(e,256);}check(rows(e)==1,"DSP restart retained old History/cadence");DataFrameView snapshot;check(e.plan()->history(7,0,*e.context(),snapshot)==Status::ok,"DSP restart snapshot");const auto& view=std::get<NumericView>(snapshot.payload);bool positive=false;for(std::size_t band=0;band<50;++band){float value=0;check(load_element(view,{0,view.actual[0]-1,band},value)==Status::ok&&std::isfinite(value)&&value>=0,"DSP restart Mel finite/nonnegative");positive|=value>0;}check(positive,"DSP restart did not resume real Mel");}
 std::puts("actual 8-tone Window/FFT/Mel/History repeated re-admission/Stop10 cycles, fresh positive finite output PASS");}
struct SelectorObservation {
 std::size_t available=0,unavailable=0,windows=0;
 FrameStamp last{};bool inspect_fresh_window=false,expect_history_discontinuity=false,expect_selector_zero=false;
 std::uint64_t history_sequence=0;
};
static void selector_output(void* user,const NumericPortFrameView& f){auto& o=*static_cast<SelectorObservation*>(user);
 if(f.identity.node==14){o.last=f.frame.stamp;
  if(f.frame.stamp.available){++o.available;check(f.status==Status::ok,"selector available status");
   if(o.expect_selector_zero){const auto& v=std::get<NumericView>(f.frame.payload);for(std::size_t p=0;p<v.planes.size();++p)for(std::size_t j=0;j<v.actual[0];++j){float x=1;check(load_element(v,{p,j,0},x)==Status::ok&&x==0,"disabled selector did not emit typed PCM zeros");}}}
  else {++o.unavailable;check(f.status==Status::unavailable&&std::get<NumericView>(f.frame.payload).storage.empty(),"selector unavailable is not available zeros");}}
 if(f.identity.node==2&&f.frame.stamp.available){++o.windows;
  if(o.inspect_fresh_window){const auto& v=std::get<NumericView>(f.frame.payload);
   for(std::size_t p=0;p<v.planes.size();++p)for(std::size_t j=0;j<v.actual[0]-800;++j){float x=1;check(load_element(v,{p,j,0},x)==Status::ok&&x==0,"selector mixed old producer into fresh Window");}
   check(f.frame.stamp.discontinuity,"first Window after selector switch lacks discontinuity");o.inspect_fresh_window=false;}}
 if(f.identity.node==7&&f.identity.port==0&&f.frame.stamp.available){
  check(f.frame.stamp.sequence==o.history_sequence+1,"selector History sequence lost continuity");o.history_sequence=f.frame.stamp.sequence;
  if(o.expect_history_discontinuity){check(f.frame.stamp.discontinuity&&f.frame.stamp.producer_epoch==o.last.producer_epoch&&f.frame.stamp.source_sequence==o.last.source_sequence&&f.frame.stamp.timestamp_ticks>=o.last.timestamp_ticks,"selector appended History discontinuity/source/timestamp lost");o.expect_history_discontinuity=false;}}
}
static void input_selector(){auto g=oracle_graph();const auto pcm=node(g,1).outputs[0].contract;
 auto physical=node(g,1);physical.id=13;physical.kind="AudioSource";physical.parameters=EndpointProcessingParameters{};g.nodes.push_back(std::move(physical));
 ConfiguredProcessingNode selector;selector.id=14;selector.kind="InputSelector";selector.parameters=TransformProcessingParameters{};
 selector.inputs={{0,PortDirection::Input,pcm},{1,PortDirection::Input,pcm}};selector.outputs={{0,PortDirection::Output,pcm}};g.nodes.push_back(std::move(selector));
 for(auto& edge:g.edges)if(edge.from==1)edge.from=14;g.edges.push_back({1,14,0,0});g.edges.push_back({13,14,0,1});
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"selector initial Apply");
 Feed generator(g),capture(g);capture.epoch=99;capture.sent=480000;SelectorObservation o;NumericRuntimeCallbacks cb{selector_output,nullptr,nullptr,&o};
 auto send=[&](NodeId id,Feed& feed,bool available=true,bool discontinuity=false){auto f=feed.frame(256);f.stamp.available=available;f.stamp.discontinuity=discontinuity;if(!available)std::get<NumericView>(f.payload).storage={};
  auto before=allocations;check(e.process_numeric_ports(id,f,{},cb)==Status::ok,"selector ingress");check(allocations==before,"selector hot allocation");return f.stamp;};
 for(int i=0;i<32;++i){send(1,generator);send(13,capture);}const auto retained=rows(e);check(retained>0&&o.available==32,"selector default input0");
 DataFrameView old;check(e.plan()->history(7,0,*e.context(),old)==Status::ok,"selector old History");std::vector<std::uint64_t> bits;
 const auto& history=std::get<NumericView>(old.payload);for(std::size_t r=0;r<retained;++r)for(std::size_t b=0;b<50;++b){std::uint64_t x=0;check(load_bits(history,{0,history.actual[0]-retained+r,b},DType::Float32,x)==Status::ok,"selector History bits");bits.push_back(x);}
 node(g,14).inputs.push_back({2,PortDirection::Input,pcm});prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"unused selector input growth Apply");
 const auto selector_sequence=o.last.sequence;send(1,generator);
 check(o.last.sequence==selector_sequence+1 && !o.last.discontinuity && rows(e)==retained,"unused input growth reset active selector clock or History");
 node(g,14).inputs.pop_back();prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"unused selector input shrink Apply");
 const auto chosen=o.available;send(13,capture,false);++capture.epoch;send(13,capture,true,true);check(o.available==chosen&&o.unavailable==0&&rows(e)==retained,"inactive source poisoned selected History");
 auto select=[&](std::uint64_t input){std::get<TransformProcessingParameters>(node(g,14).parameters).values={{"selected_input",NumericLiteral{input}}};prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"selector compatible Apply");};
 select(1);check(rows(e)==retained,"selector Apply cleared History");o.inspect_fresh_window=true;o.expect_history_discontinuity=true;
 const auto first=send(13,capture);check(o.last.discontinuity&&o.last.producer_epoch==first.producer_epoch&&o.last.source_sequence==first.source_sequence&&o.last.timestamp_ticks==first.timestamp_ticks,"selector switch metadata");
 for(int i=0;i<3;++i)send(13,capture);check(!o.inspect_fresh_window&&!o.expect_history_discontinuity&&rows(e)==retained+1,"selector switch fresh DSP row");
 DataFrameView preserved;check(e.plan()->history(7,0,*e.context(),preserved)==Status::ok,"selector retained History query");const auto& v=std::get<NumericView>(preserved.payload);
 for(std::size_t r=0;r<retained;++r)for(std::size_t b=0;b<50;++b){std::uint64_t x=0;check(load_bits(v,{0,v.actual[0]-(retained+1)+r,b},DType::Float32,x)==Status::ok&&x==bits[r*50+b],"selector mutated retained History bits");}
 auto active=o.available;for(int i=0;i<4;++i)send(1,generator);check(o.available==active,"inactive generator reached selector output");
 send(13,capture,false);DataFrameView unavailable_history;std::uint64_t unavailable_filled=0;check(o.unavailable==1&&e.plan()->history(7,0,*e.context(),unavailable_history,&unavailable_filled)==Status::unavailable&&unavailable_filled==retained+1,"selected unavailable lost retained History");o.inspect_fresh_window=true;o.expect_history_discontinuity=true;
 for(int i=0;i<4;++i)send(13,capture);check(!o.inspect_fresh_window&&!o.expect_history_discontinuity&&rows(e)==retained+2,"selector recovery DSP");
 select(0);auto resumed=send(1,generator);check(o.last.discontinuity&&o.last.producer_epoch==generator.epoch&&o.last.sequence==resumed.sequence,"selector generator did not advance independently");
 node(g,14).enabled=false;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"disabled selector Apply");o.expect_selector_zero=true;active=o.available;send(1,generator);check(o.available==active+1&&o.last.available,"disabled selector became unavailable with healthy clock");
 active=o.unavailable;send(1,generator,false);check(o.unavailable==active+1&&!o.last.available,"disabled selector hid unavailable active source");
 node(g,14).enabled=true;o.expect_selector_zero=false;
 node(g,14).inputs.push_back({2,PortDirection::Input,pcm});g.edges.push_back({1,14,0,2});
 DataFrameView before_third;std::uint64_t before_third_filled=0;const auto before_third_status=e.plan()->history(7,0,*e.context(),before_third,&before_third_filled);
 check(before_third_status==Status::ok || before_third_status==Status::unavailable,"N selector retained unavailable History query");
 const auto rows_before_third=before_third_filled;select(2);
 DataFrameView after_third;std::uint64_t after_third_filled=0;const auto after_third_status=e.plan()->history(7,0,*e.context(),after_third,&after_third_filled);
 check((after_third_status==Status::ok || after_third_status==Status::unavailable) && after_third_filled==rows_before_third,"N selector growth retains unavailable History");
 active=o.available;send(1,generator);check(o.available==active+1,"N selector third stable input delivers");
 node(g,14).inputs.pop_back();std::erase_if(g.edges,[](const auto& edge){return edge.to==14&&edge.input_port==2;});select(0);
 auto malformed=g;std::get<TransformProcessingParameters>(node(malformed,14).parameters).values={{"selected_input",NumericLiteral{std::uint64_t(2)}}};auto p=profile();check(e.prepare_numeric(malformed,{p.admission,p.resources},pending)!=Status::ok,"selector accepted input2");
 malformed=g;node(malformed,14).inputs[1].contract=node(g,6).outputs[0].contract;check(e.prepare_numeric(malformed,{p.admission,p.resources},pending)!=Status::ok,"selector accepted incompatible input");
 select(1);std::erase_if(g.edges,[](const auto& edge){return edge.to==14&&edge.input_port==1;});
 auto unrelated=node(g,13);unrelated.id=16;auto unrelated_contract=std::get<NumericContract>(pcm.view());unrelated_contract.clock={"unrelated",{1,44100},44100};
 std::get<EndpointProcessingParameters>(unrelated.parameters).values.push_back({"sample_rate",NumericLiteral{std::uint64_t{44100}}});
 check(own_processing_contract(unrelated_contract,limits,65536,unrelated.outputs[0].contract)==Status::ok,"unrelated ingress contract");g.nodes.push_back(std::move(unrelated));
 prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"selector missing selected edge Apply");o.expect_selector_zero=true;
 Feed unrelated_feed(g);unrelated_feed.contract=unrelated_contract;unrelated_feed.epoch=200;active=o.available;
 send(1,generator);send(16,unrelated_feed);check(o.available==active,"disconnected selector ticked on inactive/unrelated clock");
 const auto silence_stamp=send(13,capture);check(o.available==active+1&&o.last.available&&o.last.producer_epoch==silence_stamp.producer_epoch&&o.last.timestamp_ticks==silence_stamp.timestamp_ticks,"disconnected selected edge known PCM clock/provenance");
 DataFrameView silence_history;std::uint64_t silence_filled=0;const auto silence_status=e.plan()->history(7,0,*e.context(),silence_history,&silence_filled);check(silence_status==Status::ok||silence_status==Status::unavailable,"disconnected selector History retention");const auto silence_rows=std::size_t(silence_filled);for(int i=0;i<3;++i){send(1,generator);send(16,unrelated_feed);send(13,capture);}check(o.available==active+4&&rows(e)==silence_rows+1,"disconnected selector double cadence on alternating sources");
 GraphExecutor cold_selector(profile());prepare(cold_selector,g,pending);check(cold_selector.apply(std::move(pending))==Status::ok,"cold disconnected selector Apply");SelectorObservation cold_observation;cold_observation.expect_selector_zero=true;
 auto cold_send=[&](NodeId id,Feed& feed){auto frame=feed.frame(256);auto before=allocations;check(cold_selector.process_numeric_ports(id,frame,{}, {selector_output,nullptr,nullptr,&cold_observation})==Status::ok&&allocations==before,"cold selector clock/noalloc");};
 for(int i=0;i<4;++i){cold_send(13,capture);cold_send(16,unrelated_feed);check(cold_observation.available==std::size_t(i),"cold selector ticked on second/unrelated source");cold_send(1,generator);}
 check(cold_observation.available==4&&cold_observation.last.producer_epoch==generator.epoch&&rows(cold_selector)==1,"cold selector stable compatible clock/cadence");
 auto unknown_contract=std::get<NumericContract>(pcm.view());unknown_contract.clock={"",{1,1},0};OwnedProcessingContract unknown_pcm;
 check(own_processing_contract(unknown_contract,limits,65536,unknown_pcm)==Status::ok,"selector unknown clock contract");
 auto clockless_source=node(g,1);clockless_source.kind="NetworkInput";clockless_source.parameters=EndpointProcessingParameters{};clockless_source.outputs[0].contract=unknown_pcm;
 auto clockless_selector=node(g,14);for(auto* ports:{&clockless_selector.inputs,&clockless_selector.outputs})for(auto& port:*ports)port.contract=unknown_pcm;
 NumericProcessingGraph clockless;clockless.graph_id="selector.clockless";clockless.nodes={clockless_source,clockless_selector};clockless.edges={{1,14,0,0}};
 GraphExecutor unknown_executor(profile());prepare(unknown_executor,clockless,pending);check(unknown_executor.apply(std::move(pending))==Status::ok,"selector unknown clock Apply");
 auto unknown_frame=generator.frame(256);std::get<NumericView>(unknown_frame.payload).contract=&unknown_contract;SelectorObservation unknown_observation;
 auto before_allocations=allocations;check(unknown_executor.process_numeric_ports(1,unknown_frame,{}, {selector_output,nullptr,nullptr,&unknown_observation})==Status::ok&&allocations==before_allocations&&unknown_observation.unavailable==1&&unknown_observation.available==0,"disconnected selected edge unknown clock became available");
 auto sparse_source=node(g,1);auto sparse_selector=node(g,14);sparse_selector.inputs.resize(1);sparse_selector.inputs[0].id=65535;
 std::get<TransformProcessingParameters>(sparse_selector.parameters).values={{"selected_input",NumericLiteral{std::uint64_t(65535)}}};
 NumericProcessingGraph sparse;sparse.graph_id="selector.sparse";sparse.nodes={sparse_source,sparse_selector};sparse.edges={{1,14,0,65535}};
 GraphExecutor sparse_executor(profile());prepare(sparse_executor,sparse,pending);check(sparse_executor.apply(std::move(pending))==Status::ok,"sparse selector Apply");
 auto sparse_frame=generator.frame(256);SelectorObservation sparse_observation;const auto sparse_before=allocations;
 check(sparse_executor.process_numeric_ports(1,sparse_frame,{}, {selector_output,nullptr,nullptr,&sparse_observation})==Status::ok && allocations==sparse_before && sparse_observation.available==1,"sparse selector stable ID/no zero anchor/noalloc");
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
static void grouped_selector() {
 auto base=oracle_graph();geometry(base,48000,4096,317);NumericProcessingGraph g;g.graph_id="grouped-selector";
 for(int id=1;id<=4;++id){auto source=node(base,1);source.id=id;g.nodes.push_back(std::move(source));}
 const auto pcm=g.nodes.front().outputs.front().contract;
 ConfiguredProcessingNode selector;selector.id=20;selector.kind="InputSelector";
 selector.parameters=TransformProcessingParameters{{{"selected_input",NumericLiteral{std::uint64_t{0}}},{"channels_per_stream",NumericLiteral{std::uint64_t{2}}}}};
 for(unsigned p=0;p<4;++p)selector.inputs.push_back({PortId(p),PortDirection::Input,pcm});
 auto envelope=pcm.view();std::get<NumericContract>(envelope).shape.extents[0]={0,512};OwnedProcessingContract bounded;
 check(own_processing_contract(envelope,limits,65536,bounded)==Status::ok,"group envelope");
 for(unsigned p=0;p<2;++p)selector.outputs.push_back({PortId(p),PortDirection::Output,bounded});g.nodes.push_back(std::move(selector));
 g.edges={{1,20,0,0},{2,20,0,1},{3,20,0,2},{4,20,0,3}};
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"group initial Apply");
 std::array<Feed,4> feeds{Feed(g),Feed(g),Feed(g),Feed(g)};GroupObservation o;
 auto send=[&](unsigned id,std::size_t count=256){auto& feed=feeds[id-1];feed.epoch=100+id;auto frame=feed.frame(count);std::fill(feed.samples.begin(),feed.samples.end(),float(id));
  const auto before=allocations;check(e.process_numeric_ports(id,frame,{}, {group_output,nullptr,nullptr,&o})==Status::ok&&allocations==before,"group synchronous/noalloc");};
 for(int tick=0;tick<3;++tick){send(1);send(2);send(3);send(4);}
 check(o.calls==std::array<unsigned,2>{3,3}&&o.values==std::array<float,2>{1,2}&&!o.discontinuity[0]&&!o.discontinuity[1],"group active lanes only / independent continuity");
 std::get<TransformProcessingParameters>(node(g,20).parameters).values[0].value=NumericLiteral{std::uint64_t{1}};
 prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"group switch Apply");send(1);send(2);check(o.calls==std::array<unsigned,2>{3,3},"old stream still routed");
 send(3);send(4);check(o.values==std::array<float,2>{3,4}&&o.discontinuity[0]&&o.discontinuity[1],"all lanes switch and mark boundary");
 send(3);send(4);check(!o.discontinuity[0]&&!o.discontinuity[1],"group boundary persisted across frames");
 std::erase_if(g.edges,[](const auto& edge){return edge.from==4;});prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"group missing lane Apply");
 const auto calls=o.calls;send(3,317);check(o.calls[0]==calls[0]+1&&o.calls[1]==calls[1]+1&&o.values[0]==3&&o.values[1]==0,"missing channel typed zero, no mono duplication");
 check(o.sizes[0]==317&&o.sizes[1]==317&&o.ticks[0]==o.ticks[1],"missing channel chunk size and timestamp differ");
 send(1);send(2);send(4);check(o.calls[0]==calls[0]+1&&o.calls[1]==calls[1]+1,"missing lane double tick");
 // Discovery can leave a disconnected resampler in front of a missing lane.
 auto orphan=node(base,1);orphan.id=30;
 orphan.kind="PcmResampler";orphan.inputs={{0,PortDirection::Input,pcm}};
 orphan.parameters=TransformProcessingParameters{{{"output_rate",NumericLiteral{std::uint64_t{48000}}}}};
 auto resampled=pcm.view();std::get<NumericContract>(resampled).shape.extents[0]={1,318};OwnedProcessingContract resampled_owner;
 check(own_processing_contract(resampled,limits,65536,resampled_owner)==Status::ok,"orphan resampler output");orphan.outputs={{0,PortDirection::Output,resampled_owner}};
 node(g,20).inputs[3].contract=resampled_owner;g.nodes.push_back(std::move(orphan));g.edges.push_back({30,20,0,3});
 GraphExecutor cold(profile());prepare(cold,g,pending);check(cold.apply(std::move(pending))==Status::ok,"cold missing physical lane Apply");
 auto frame=feeds[2].frame(123);std::fill(feeds[2].samples.begin(),feeds[2].samples.end(),3.f);const auto before=allocations;
 check(cold.process_numeric_ports(3,frame,{}, {group_output,nullptr,nullptr,&o})==Status::ok&&allocations==before,"cold orphan noalloc");
 check(o.values[1]==0&&o.sizes[0]==123&&o.sizes[1]==123&&o.ticks[0]==o.ticks[1],"cold orphan channel did not follow selected chunk");
 std::puts("Grouped selector: two streams/two mono lanes/atomic selection/continuity/missing silence/noalloc PASS");
}

struct AlignedObserved {
 std::array<float,8> values{};std::array<float,317> samples{};std::array<unsigned,8> calls{};std::size_t count=0;FrameStamp stamp{};unsigned unavailable=0;
};
static void aligned_output(void* user,const NumericPortFrameView& frame){if(frame.identity.node!=20)return;auto& out=*static_cast<AlignedObserved*>(user);
 if(!frame.frame.stamp.available){++out.unavailable;return;}check(validate_view(frame.frame,limits)==Status::ok,"aligned output valid");
 const auto& value=std::get<NumericView>(frame.frame.payload);const auto p=frame.identity.port;check(p<out.values.size(),"aligned output port");
 check(load_element(value,{0,0,0},out.values[p])==Status::ok,"aligned output value");
 if(value.contract->shape.rank==Rank::Vector)for(std::size_t i=0;i<value.actual[0];++i){check(i<out.samples.size(),"aligned bounded observation");check(load_element(value,{0,i,0},out.samples[i])==Status::ok,"aligned full payload observation");}
 ++out.calls[p];out.count=value.actual[0];out.stamp=frame.frame.stamp;out.stamp.stream_id={};}
static NumericProcessingGraph aligned_fixture(std::size_t lanes,bool comparison=false,bool routing=false){auto base=oracle_graph();geometry(base,48000,4096,317);
 auto source=node(base,1);source.kind="AudioSource";source.parameters=EndpointProcessingParameters{};auto pcm=source.outputs.front().contract;source.outputs.clear();
 for(std::size_t lane=0;lane<lanes;++lane)source.outputs.push_back({PortId(lane),PortDirection::Output,pcm});
 NumericProcessingGraph g;g.graph_id="aligned.regression";g.nodes.push_back(source);ConfiguredProcessingNode target;target.id=20;target.kind=routing?"Switch":comparison?"ChannelComparison":"Mixer";
 for(std::size_t lane=0;lane<lanes;++lane){target.inputs.push_back({PortId(lane),PortDirection::Input,pcm});g.edges.push_back({1,20,PortId(lane),PortId(lane)});}
 if(routing){target.parameters=RouteProcessingParameters{3,1,true,2};for(PortId p=0;p<6;++p)target.outputs.push_back({p,PortDirection::Output,pcm});}
 else if(comparison){target.parameters=TransformProcessingParameters{{{"window_samples",NumericLiteral{std::uint64_t{512}}},{"silence_threshold",NumericLiteral{1e-12}}}};
  auto c=std::get<NumericContract>(pcm.view());c.semantic=Semantic::Scalar;c.shape.rank=Rank::Scalar;c.shape.axes={Axis::None,Axis::None};c.shape.extents={ExtentBounds{1,1},ExtentBounds{1,1}};c.coordinates={};
  OwnedProcessingContract scalar;check(own_processing_contract(c,limits,65536,scalar)==Status::ok,"comparison scalar own");target.outputs={{0,PortDirection::Output,scalar}};}
 else {MixerProcessingParameters params;params.mode=MixerMode::WeightedMean;params.consumption=MixerConsumption::SourceAligned;params.weights.clear();
  for(std::size_t lane=0;lane<lanes;++lane)params.weights.push_back({PortId(lane),1});target.parameters=params;target.outputs={{0,PortDirection::Output,pcm}};}
 g.nodes.push_back(target);return g;
}
static void aligned_regressions(){auto p=profile();p.admission.max_ports_per_node=16;
 // Ten independent 100ms sample/provenance FIFOs exceed the small mono
 // oracle payload budget. Keep the queue capacity bounded; provision the
 // test device explicitly for this N-input resource scenario.
 p.resources.maximum.payload_bytes=4*1048576;p.resources.max_peak_bytes=16*1048576;
 auto activate=[&](GraphExecutor& e,const NumericProcessingGraph& g){application::PreparedExecution pending;const auto status=e.prepare_numeric(g,{p.admission,p.resources},pending);if(status!=Status::ok){std::fprintf(stderr,"aligned prepare status=%d nodes=%zu ports=%zu payload_budget=%zu peak_budget=%zu\n",int(status),g.nodes.size(),g.nodes.front().outputs.size(),p.resources.maximum.payload_bytes,p.resources.max_peak_bytes);
  std::vector<std::size_t> workspace(g.nodes.size()*2);ProcessingGraphAdmission admission;const auto diagnostic=admit_processing_graph(g,p.admission,PreparedGraph::numeric_capabilities(p),workspace,admission);
  std::fprintf(stderr,"aligned admission error=%d node=%d port=%u payload=%zu peak=%zu\n",int(diagnostic.error),int(diagnostic.node),unsigned(diagnostic.port),admission.resources.payload_bytes,admission.peak_bytes);}
check(status==Status::ok&&e.apply(std::move(pending))==Status::ok,"aligned Apply");};
 auto g=aligned_fixture(10);GraphExecutor e(p);activate(e,g);Feed feed(g);AlignedObserved out;
 auto send=[&](GraphExecutor& executor,PortId lane,std::int64_t begin,std::size_t count,float value,bool discontinuity=false,Ratio latency={0,1}){auto frame=feed.frame(count);std::fill(feed.samples.begin(),feed.samples.end(),value);frame.stamp.sample_begin=begin;frame.stamp.sample_count=count;frame.stamp.has_sample_interval=true;frame.stamp.timestamp_ticks=begin+std::int64_t(count)-1;frame.stamp.discontinuity=discontinuity;frame.stamp.latency_seconds=latency;
  const auto before=allocations;const auto status=executor.process_numeric_ports(1,lane,frame,{}, {aligned_output,nullptr,nullptr,&out});check(status==Status::ok&&allocations==before,"aligned processing noalloc");};
 for(PortId lane=0;lane<9;++lane)send(e,lane,0,127,float(lane+1));check(out.calls[0]==0,"aligned cannot emit partial inputs");send(e,9,0,127,10);check(out.calls[0]==1&&out.count==127&&out.values[0]==5.5f&&out.stamp.sample_begin==0,"ten-input aligned mean");
 for(PortId lane=0;lane<10;++lane)send(e,lane,127,193,float(lane+1));check(out.calls[0]==2&&out.stamp.sample_begin==127&&!out.stamp.discontinuity,"irregular aligned continuity");
 // One lane skips 127 samples. Others' obsolete positions must be discarded.
 send(e,0,447,127,1);for(PortId lane=1;lane<10;++lane)send(e,lane,320,254,float(lane+1));check(out.calls[0]==3&&out.stamp.sample_begin==447&&out.stamp.discontinuity,"asymmetric gap matches positions");
 auto delayed=aligned_fixture(2);GraphExecutor latency(p);activate(latency,delayed);out={};send(latency,0,0,123,2);send(latency,1,10,123,4,false,{10,48000});
 check(out.calls[0]==1&&out.values[0]==3&&out.stamp.sample_begin==10&&out.stamp.latency_samples==10,"integer latency aligns content positions");
 // Queued contiguous blocks may change latency without changing generation.
 // A head-only match must not consume the incompatible tail as one frame.
 auto fractional_graph=aligned_fixture(2);GraphExecutor fractional(p);activate(fractional,fractional_graph);out={};
 send(fractional,0,0,4,1);send(fractional,0,4,4,8,false,{1,96000});send(fractional,1,0,8,2);
 check(out.calls[0]==1&&out.count==4&&out.values[0]==1.5f&&out.stamp.sample_begin==0,"fractional latency change preserves only aligned prefix");
 const auto fractional_stats=fractional.mixer_input_stats(20,0);check(fractional_stats&&fractional_stats->dropped_samples>=4,"fractional unmatched tail discarded");
 send(fractional,0,8,4,4);send(fractional,1,8,4,2);
 check(out.calls[0]==2&&out.count==4&&out.values[0]==3&&out.stamp.sample_begin==8&&out.stamp.discontinuity,"fractional tail reset recovers without stale data");
 auto integer_graph=aligned_fixture(2);GraphExecutor integer_jump(p);activate(integer_jump,integer_graph);out={};
 send(integer_jump,0,0,4,1);send(integer_jump,0,4,4,8,false,{1,48000});send(integer_jump,1,0,8,2);
 check(out.calls[0]==2&&out.count==3&&out.values[0]==5&&out.stamp.sample_begin==5&&out.stamp.discontinuity,"integer latency change splits prefix and matches shifted tail");
 auto one=aligned_fixture(1);auto& one_params=std::get<MixerProcessingParameters>(node(one,20).parameters);one_params.mode=MixerMode::WeightedSum;one_params.weights[0].weight=3;
 GraphExecutor single(p);activate(single,one);out={};send(single,0,0,123,2);check(out.calls[0]==1&&out.values[0]==6,"N1 weighted sum has no hidden clipping");
 auto anti=aligned_fixture(2);GraphExecutor cancellation(p);activate(cancellation,anti);out={};send(cancellation,0,0,123,2);send(cancellation,1,0,123,-2);check(out.values[0]==0,"aligned antiphase cancellation");
 auto bounded=aligned_fixture(2);GraphExecutor overflow(p);activate(overflow,bounded);out={};for(std::int64_t begin=0;begin<10240;begin+=256)send(overflow,0,begin,256,2);
 send(overflow,1,9600,256,4);const auto queue_stats=overflow.mixer_input_stats(20,0);check(out.calls[0]==1&&out.stamp.sample_begin==9600&&out.stamp.discontinuity&&queue_stats&&queue_stats->dropped_samples>0&&queue_stats->capacity==4800,"aligned overflow bounded/resynchronizes by sample position");
 // Generation/continuity changes never blend old queues, even at equal positions.
 auto comparison=aligned_fixture(2,true);GraphExecutor balance(p);activate(balance,comparison);out={};
 for(std::int64_t begin=0;begin<512;begin+=256){send(balance,0,begin,256,1);send(balance,1,begin,256,1);}check(out.calls[0]==1&&out.values[0]==0&&out.stamp.sample_count==512,"equal energy balance");
 for(std::int64_t begin=512;begin<1024;begin+=256){send(balance,0,begin,256,0);send(balance,1,begin,256,1);}check(out.calls[0]==2&&out.values[0]==1,"only channel2 balance");
 for(std::int64_t begin=1024;begin<1536;begin+=256){send(balance,0,begin,256,1);send(balance,1,begin,256,0);}check(out.values[0]==-1,"only channel1 balance");
 for(std::int64_t begin=1536;begin<2048;begin+=256){send(balance,0,begin,256,0);send(balance,1,begin,256,0);}check(out.values[0]==0,"silent balance");
 send(balance,0,2304,256,1,true);check(out.unavailable>0,"comparison gap clears displayed metric");
 const auto previous_calls=out.calls[0];send(balance,0,0,256,1,true);send(balance,1,0,256,1,true);send(balance,0,256,256,1);send(balance,1,256,256,1);
 check(out.calls[0]==previous_calls+1&&out.values[0]==0&&out.stamp.sample_begin==0&&out.stamp.discontinuity,"paired discontinuity preserves new first interval");
 auto route=aligned_fixture(2,false,true);GraphExecutor sw(p);activate(sw,route);out={};send(sw,0,0,123,3);send(sw,1,0,123,7);
 check(out.calls==std::array<unsigned,8>{1,1,1,1,1,1,0,0}&&out.values==std::array<float,8>{0,0,3,7,0,0,0,0},"grouped output switch maps lane1-to1 lane2-to2");
 std::puts("SourceAligned N10/irregular/gap, comparison balance/silence/invalidation, grouped Switch/noalloc PASS");
}
static void startup_preparation_oracle(){
 auto g=numeric_startup_graph();
 // Match the desktop engine's actual admission/kernel/reservation profile;
 // generous generic oracle budgets can conceal composition differences.
 constexpr std::size_t fixed=256*(262144+16384)+(384+48)*1024*1024;
 NumericRuntimeProfile p{{512,2048,8,32,256,4*1024*1024,limits},
  {{16*1024*1024,32*1024*1024,16*1024*1024,1024*1024},fixed,576*1024*1024},
  {32768,8,262144,262144,16*1024*1024},{true,true,true},true};
 std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
 const auto status=PreparedGraph::prepare_numeric(g,p,plan,context);
 if(status!=Status::ok){
  std::fprintf(stderr,"startup prepare status=%d nodes=%zu\n",int(status),g.nodes.size());
  // Induce each node's complete data/configuration ancestry; missing unrelated
  // consumers cannot affect preparation. This identifies the smallest failing
  // prepared fragment without bypassing any admission requirement.
  for(const auto& target:g.nodes){auto fragment=g;std::vector<NodeId> keep{target.id};
   for(size_t i=0;i<keep.size();++i){const auto add=[&](NodeId id){if(std::find(keep.begin(),keep.end(),id)==keep.end())keep.push_back(id);};
    for(const auto& edge:g.edges)if(edge.to==keep[i])add(edge.from);
    for(const auto& binding:g.configuration_bindings)if(binding.to==keep[i])add(binding.from);}
   const auto retained=[&](NodeId id){return std::find(keep.begin(),keep.end(),id)!=keep.end();};
   std::erase_if(fragment.nodes,[&](const auto& n){return !retained(n.id);});
   std::erase_if(fragment.edges,[&](const auto& e){return !retained(e.from)||!retained(e.to);});
   std::erase_if(fragment.configuration_bindings,[&](const auto& b){return !retained(b.from)||!retained(b.to);});
   std::shared_ptr<const PreparedGraph> part;std::unique_ptr<RuntimeContext> state;
   const auto failed=PreparedGraph::prepare_numeric(fragment,p,part,state);
   if(failed!=Status::ok)std::fprintf(stderr,"startup ancestor target=%d kind=%s status=%d nodes=%zu\n",int(target.id),target.kind.c_str(),int(failed),fragment.nodes.size());
  }
 }
 check(status==Status::ok,"startup complete stereo preparation");
 // Native 48kHz mono discovery preserves these factories' clock/geometry,
 // removing only unavailable physical output1 and its data edge. Exercise the
 // resulting disconnected resampler lanes without changing explicit bindings.
 for(const NodeId source_id:{13,14}){
  auto& physical=node(g,source_id);check(physical.outputs.size()==2,"startup capture fixture stereo before discovery");physical.outputs.resize(1);
  std::erase_if(g.edges,[&](const auto& edge){return edge.from==source_id && edge.output_port>=1;});
 }
 for(std::uint64_t selected=0;selected<3;++selected){
  auto& selector=std::get<TransformProcessingParameters>(node(g,200).parameters);
  for(auto& field:selector.values)if(field.key=="selected_input")field.value=NumericLiteral{selected};
  std::shared_ptr<const PreparedGraph> mono_plan;std::unique_ptr<RuntimeContext> mono_state;
  const auto mono_status=PreparedGraph::prepare_numeric(g,p,mono_plan,mono_state);
  if(mono_status!=Status::ok)std::fprintf(stderr,"startup mono48000 selected=%llu status=%d\n",(unsigned long long)selected,int(mono_status));
  check(mono_status==Status::ok,"startup mono discovery preserves explicit bindings and disconnected physical lanes");
 }
}
#include "aligned_numerical_acceptance.h"
static void moved_from_context_rejected(){
 auto graph=oracle_graph();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
 check(PreparedGraph::prepare_numeric(graph,profile(),plan,context)==Status::ok,"moved context prepare");
 Feed feed(graph);auto input=feed.frame(256);RuntimeContext moved(std::move(*context));
 const auto before=allocations;
 check(plan->process_numeric_ports(1,input,*context,{})==Status::invalid_argument,"moved-from context rejected before dereference");
 check(plan->process_numeric_ports(1,0,input,*context,{})==Status::invalid_argument,"explicit port rejects moved-from context");
 check(allocations==before,"moved-from context rejection allocates");
 check(plan->process_numeric_ports(1,input,moved,{})==Status::ok,"moved context retains prepared ownership");
}
int main(){moved_from_context_rejected();startup_preparation_oracle();check(numeric_test_graph().execution_interval==Ratio(1,120),"new graph default remains120Hz");aligned_regressions();aligned_acceptance::resampler_range();aligned_acceptance::impulse_and_generation();aligned_acceptance::group_revision();aligned_acceptance::deadline_regressions();aligned_acceptance::physical_grid_matrix();aligned_acceptance::stereo_windows_and_mix();core_regression::run();independent_windows();latest_state_at_apply();selective_reset();source_fences();ring_capacity_and_restart();repeated_dsp_restart();input_selector();grouped_selector();}
