#include "canonical_runtime_test_fixture.h"
#include "canonical_core_regressions.h"
#include <bit>
using namespace canonical_test;
static std::array<std::byte,50*8> mel_coordinates(){
 std::array<std::byte,50*8> bytes{};
 for(std::size_t band=0;band<50;++band){const auto bits=std::bit_cast<std::uint64_t>(double(band+1)*100.);for(unsigned byte=0;byte<8;++byte)bytes[band*8+byte]=std::byte((bits>>(byte*8))&255);}
 return bytes;
}
struct Spectra{std::array<std::size_t,3> count{};std::array<std::int64_t,3> end{};std::array<std::size_t,3> peak{};};
static void spectrum(void* user,const NumericPortFrameView& f){if(f.status!=Status::ok)return;auto& result=*static_cast<Spectra*>(user);if(f.identity.node!=5&&f.identity.node!=24)return;const auto& view=std::get<NumericView>(f.frame.payload);const auto channels=view.contract->channel_order.size();
 for(std::size_t ch=0;ch<channels;++ch){const auto slot=f.identity.node==5?0:ch+1;std::size_t peak=0;float maximum=-1;
  for(std::size_t i=0;i<view.actual[0];++i){float value=0;check(load_element(view,{ch,i,0},value)==Status::ok&&std::isfinite(value),"FFT finite");if(value>maximum){maximum=value;peak=i;}}
  result.peak[slot]=peak;++result.count[slot];result.end[slot]=f.frame.stamp.timestamp_ticks;}}
static void independent_windows(){auto g=numeric_test_graph();std::erase_if(g.nodes,[](const auto& n){return n.id>5;});g.edges={{1,2,0,0},{2,3,0,0},{3,4,0,0},{4,5,0,0}};geometry(g,8192,2048,256);
 auto short_graph=numeric_test_graph();std::erase_if(short_graph.nodes,[](const auto& n){return n.id>4;});geometry(short_graph,8192,512,256);
 for(auto n:short_graph.nodes){if(n.id!=1){n.id+=20;g.nodes.push_back(std::move(n));}}g.edges.push_back({1,22,0,0});g.edges.push_back({22,23,0,0});g.edges.push_back({23,24,0,0});
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"apply dual windows");Feed feed(g);Spectra result;
 for(int i=0;i<32;++i){feed.process(e,256,{spectrum,nullptr,nullptr,&result});}for(std::size_t i=0;i<3;++i){check(result.count[i]==60&&result.end[i]==8192&&result.peak[i]==(i==0?128:i==1?32:64),"independent window schedule/timestamp");}std::puts("independent windows2048/512 stereo peaks128/32/64 PASS");}
static void latest_state_at_apply(){auto g=numeric_test_graph();GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"initial apply");Feed feed(g);for(int i=0;i<32;++i)feed.process(e,256);
 auto changed=g;node(changed,9).enabled=false;prepare(e,changed,pending);for(int i=0;i<8;++i)feed.process(e,256);const auto before=rows(e);check(e.apply(std::move(pending))==Status::ok&&rows(e)==before,"Apply copied stale Prepare history");
 prepare(e,g,pending);check(e.shutdown()==Status::ok,"shutdown pending");check(e.apply(std::move(pending))==Status::invalid_argument&&!e.running(),"shutdown fences pending Apply");check(restart(e,g)==Status::ok,"re-admit after shutdown");for(int i=0;i<4;++i){feed.process(e,256);}check(rows(e)==1,"shutdown resurrected old History");
 prepare(e,g,pending);GraphExecutor foreign(profile());check(foreign.apply(std::move(pending))==Status::invalid_argument&&bool(pending),"foreign executor consumes bundle");check(e.apply(std::move(pending))==Status::ok,"owner Apply after foreign reject");
 for(int i=0;i<8;++i){feed.process(e,256);}check(rows(e)>1,"restart accumulation");++feed.epoch;for(int i=0;i<4;++i){feed.process(e,256);}check(rows(e)==1,"producer change did not reset source descendants");std::puts("late Apply/latest state, shutdown fence, foreign bundle, producer reset PASS");}
static void selective_reset(){auto base=numeric_test_graph();auto g=base;
 for(auto n:base.nodes){n.id+=20;g.nodes.push_back(std::move(n));}for(auto edge:base.edges){edge.from+=20;edge.to+=20;g.edges.push_back(edge);}
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"two branches Apply");Feed left(base),right(base);
 auto process_right=[&](){auto f=right.frame(256);auto before=allocations;check(e.process_numeric_ports(21,f,{}, {})==Status::ok&&allocations==before,"right branch process/noalloc");};
 for(int i=0;i<32;++i){left.process(e,256);process_right();}const auto original=rows(e,7);check(rows(e,27)==original,"two branches counts");
 ++right.epoch;for(int i=0;i<4;++i){process_right();}check(rows(e,27)==1&&rows(e,7)==original,"source restart reset independent branch");
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
static void source_fences(){auto g=numeric_test_graph();auto input=node(g,6).outputs[0].contract;
 auto& source=node(g,1);source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs[0].contract=input;
 std::erase_if(g.nodes,[](const auto& n){return n.id!=1&&n.id!=7;});g.edges={{1,7,0,0}};GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"marker Apply");Feed feed(g);const auto frequencyBytes=mel_coordinates();
 auto send=[&](std::uint64_t epoch,std::uint64_t gen,std::uint64_t seq,bool discontinuity=false,bool available=true){auto f=feed.frame(50);std::get<NumericView>(f.payload).planes=std::span(feed.planes).first(1);std::get<NumericView>(f.payload).coordinates.axes[0]=frequencyBytes;f.stamp.producer_epoch=epoch;f.stamp.generation=gen;f.stamp.sequence=seq;f.stamp.discontinuity=discontinuity;f.stamp.available=available;if(!available)std::get<NumericView>(f.payload).storage={};const auto before=allocations;auto status=e.process_numeric_ports(1,f,{});check(allocations==before,"marker hot allocation");return status;};
 check(send(17,0,100)==Status::ok&&send(17,0,101)==Status::ok&&rows(e)==2,"generation zero baseline");
 check(send(17,1,1)==Status::ok&&rows(e)==1,"generation transition sequence restart");check(send(17,0,102)==Status::would_block&&rows(e)==1,"old generation accepted");
 check(send(99,1,1)==Status::ok&&rows(e)==1,"new producer sequence restart");check(send(17,1,103)==Status::would_block&&rows(e)==1,"retired producer accepted");
 check(send(99,1,2,true)==Status::ok&&rows(e)==1,"explicit discontinuity");check(send(99,1,2,true)==Status::would_block&&rows(e)==1,"duplicate discontinuity resets twice");
 check(send(99,2,0,false,false)==Status::ok,"unavailable generation marker");check(send(99,1,3)==Status::would_block,"old data revived marker");check(send(99,2,3)==Status::ok&&rows(e)==1,"current generation recovery");std::puts("generation0/restart/retired producer/duplicate discontinuity/unavailable fences PASS");}
// Retained ring and restart predicates from the old mixed runtime_tests fixture.
// Capacity edits preserve newest real rows and extend only the older typed-zero tail.
static void ring_capacity_and_restart(){
 auto g=numeric_test_graph();auto contract=node(g,6).outputs[0].contract;
 auto& source=node(g,1);source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};source.outputs[0].contract=contract;
 std::erase_if(g.nodes,[](const auto& n){return n.id!=1&&n.id!=7;});g.edges={{1,7,0,0}};
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
static void repeated_dsp_restart(){auto g=numeric_test_graph();GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"DSP restart Apply");Feed feed(g);
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
static void input_selector(){auto g=numeric_test_graph();const auto pcm=node(g,1).outputs[0].contract;
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
int main(){core_regression::run();independent_windows();latest_state_at_apply();selective_reset();source_fences();ring_capacity_and_restart();repeated_dsp_restart();input_selector();}
