#pragma once
#include <dsp_kernels/pcm_resampler.h>
// Analytical oracles for the portable executor. All observation storage is
// prepared before processing; callbacks inspect borrowed frames synchronously.
namespace aligned_acceptance {
using namespace canonical_test;
inline void resampler_range(){dsp_kernels::PcmResampler resampler;
 check(resampler.advance_input(INT64_MAX,0)&&resampler.input_contiguous(INT64_MAX),"resampler zero count at signed upper limit");
 check(!resampler.advance_input(INT64_MAX,1)&&resampler.input_contiguous(INT64_MAX),"resampler signed addition overflow leaves state unchanged");
 check(resampler.advance_input(INT64_MIN,1)&&resampler.input_contiguous(INT64_MIN+1),"resampler negative timestamp range");
 const auto maximum=std::numeric_limits<std::size_t>::max();
 if(std::in_range<std::int64_t>(maximum))check(resampler.advance_input(0,maximum)&&resampler.input_contiguous(std::int64_t(maximum)),"resampler size_t32 full range");
 else check(!resampler.advance_input(0,maximum)&&resampler.input_contiguous(INT64_MIN+1),"resampler size_t64 out-of-signed-range refusal");
}
inline void impulse_and_generation(){
 auto g=aligned_fixture(2);GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"impulse aligned Apply");Feed feed(g);AlignedObserved out;
 auto send=[&](PortId lane,std::int64_t begin,std::uint64_t generation,float base,bool impulse){auto frame=feed.frame(127);feed.samples.fill(base);if(impulse)feed.samples[63]+=2;
  frame.stamp.generation=generation;frame.stamp.has_sample_interval=true;frame.stamp.sample_begin=begin;frame.stamp.sample_count=127;frame.stamp.timestamp_ticks=begin+126;
  const auto before=allocations;check(e.process_numeric_ports(1,lane,frame,{},{aligned_output,nullptr,nullptr,&out})==Status::ok&&allocations==before,"impulse/generation hot path noalloc");};
 send(0,0,4,0,true);check(out.calls[0]==0,"delayed lane cannot prematurely emit impulse");send(1,0,4,0,false);
 check(out.calls[0]==1&&out.count==127,"impulse paired output");for(std::size_t i=0;i<127;++i)check(out.samples[i]==(i==63?1.f:0.f),"impulse preserves exact sample index and mean");
 // An old unmatched tail exists when the whole source advances generation.
 send(0,127,4,9,false);send(0,0,5,2,false);check(out.calls[0]==1,"new generation waits for both lanes");send(1,0,5,4,false);
 check(out.calls[0]==2&&out.stamp.generation==5&&out.stamp.sample_begin==0&&out.stamp.discontinuity,"paired generation replaces old cohort");for(std::size_t i=0;i<127;++i)check(out.samples[i]==3,"generation excludes every old queued sample");
 std::puts("aligned impulse/full-vector/delayed lane/source generation/noalloc PASS");
}
inline void group_revision(){auto g=aligned_fixture(2);auto second=node(g,1);second.id=2;g.nodes.push_back(second);const auto pcm=node(g,1).outputs[0].contract;
 ConfiguredProcessingNode selector;selector.id=30;selector.kind="InputSelector";selector.parameters=TransformProcessingParameters{{{"selected_input",NumericLiteral{std::uint64_t{0}}},{"channels_per_stream",NumericLiteral{std::uint64_t{2}}}}};
 for(PortId port=0;port<4;++port)selector.inputs.push_back({port,PortDirection::Input,pcm});for(PortId port=0;port<2;++port)selector.outputs.push_back({port,PortDirection::Output,pcm});g.nodes.push_back(selector);
 g.edges={{1,30,0,0},{1,30,1,1},{2,30,0,2},{2,30,1,3},{30,20,0,0},{30,20,1,1}};
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"group revision initial Apply");Feed feed(g);AlignedObserved out;
 auto send=[&](NodeId source,PortId lane,std::int64_t begin,float value){auto frame=feed.frame(127);feed.samples.fill(value);frame.stamp.has_sample_interval=true;frame.stamp.sample_begin=begin;frame.stamp.sample_count=127;frame.stamp.timestamp_ticks=begin+126;
  const auto before=allocations;check(e.process_numeric_ports(source,lane,frame,{},{aligned_output,nullptr,nullptr,&out})==Status::ok&&allocations==before,"group revision ingress noalloc");};
 send(1,0,0,2);send(1,1,0,4);check(out.calls[0]==1&&out.values[0]==3,"old group matched mean");send(1,0,127,100);
 std::get<TransformProcessingParameters>(node(g,30).parameters).values[0].value=NumericLiteral{std::uint64_t{1}};
 prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"one Apply switches entire group");send(2,0,0,6);send(2,1,0,8);
 check(out.calls[0]==2&&out.stamp.discontinuity,"group Apply emits new discontinuity");for(std::size_t i=0;i<127;++i)check(out.samples[i]==7,"group Apply cannot mix queued old lane with new source");
 std::puts("selector whole-group single Apply/aligned stale-tail exclusion/noalloc PASS");
}
struct DeadlineObservation {std::size_t samples=0;unsigned calls=0,unavailable=0;bool discontinuity=false;float expected=3;};
inline void deadline_output(void* user,const NumericPortFrameView& frame){if(frame.identity.node!=20)return;auto& out=*static_cast<DeadlineObservation*>(user);
 if(!frame.frame.stamp.available){++out.unavailable;return;}const auto& view=std::get<NumericView>(frame.frame.payload);++out.calls;out.samples+=view.actual[0];out.discontinuity=frame.frame.stamp.discontinuity;
 for(std::size_t i=0;i<view.actual[0];++i){float actual=0;check(load_element(view,{0,i,0},actual)==Status::ok&&actual==out.expected,"deadline exact paired payload");}
}
inline NumericProcessingGraph deadline_graph(bool comparison=false){auto g=aligned_fixture(2,comparison);
 for(auto& n:g.nodes)for(auto* ports:{&n.inputs,&n.outputs})for(auto& port:*ports){auto c=std::get<NumericContract>(port.contract.view());if(c.shape.rank!=Rank::Vector)continue;c.shape.extents[0]={0,8192};port.contract=core_regression::own(c);}return g;
}
struct DeadlineFeed {NumericContract contract;std::vector<float> samples=std::vector<float>(8192);PlaneLayout plane{0,{4,0}};std::uint64_t sequence=0;
 explicit DeadlineFeed(const NumericProcessingGraph& g):contract(std::get<NumericContract>(g.nodes[0].outputs[0].contract.view())){}
 void send(GraphExecutor& e,DeadlineObservation& out,PortId lane,std::int64_t begin,std::size_t count,float value,std::uint64_t generation=4,Ratio latency={0,1}){
  std::fill(samples.begin(),samples.end(),value);FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=17;stamp.generation=generation;stamp.has_sample_interval=true;stamp.sample_begin=begin;stamp.sample_count=count;stamp.timestamp_ticks=begin+std::int64_t(count)-1;stamp.latency_seconds=latency;
  const auto before=allocations;check(e.process_numeric_ports(1,lane,{stamp,NumericView{&contract,std::as_bytes(std::span(samples)),std::span(&plane,1),{count,1},{},{}}},{},{deadline_output,nullptr,nullptr,&out})==Status::ok&&allocations==before,"deadline ingress noalloc");
 }
};
inline void deadline_regressions(){auto g=deadline_graph();
 auto activate=[&](GraphExecutor& e,const auto& graph){application::PreparedExecution pending;prepare(e,graph,pending);check(e.apply(std::move(pending))==Status::ok,"deadline Apply");};
 GraphExecutor boundary(profile());activate(boundary,g);DeadlineFeed feed(g);DeadlineObservation out;
 feed.send(boundary,out,0,0,4800,2);feed.send(boundary,out,0,4800,1,2);feed.send(boundary,out,1,0,1,4);
 check(out.samples==1&&boundary.mixer_input_stats(20,0)->underrun_samples==0,"exact100ms boundary still matches");
 GraphExecutor expired(profile());activate(expired,g);DeadlineFeed late(g);out={};
 late.send(expired,out,0,0,4800,2);late.send(expired,out,0,4800,2,2);late.send(expired,out,0,4802,1,2);
 check(expired.mixer_input_stats(20,0)->underrun_samples==2,"source deadline expires oldest unmatched prefix before capacity overflow");
 late.send(expired,out,1,0,2,4);check(out.calls==0,"late expired positions cannot revive");late.send(expired,out,1,2,127,4);
 check(out.samples==127&&out.discontinuity,"deadline recovers at matching live positions");
 // New generations reset watermark; old high-water cannot expire restarted data.
 late.send(expired,out,0,0,127,2,5);late.send(expired,out,1,0,127,4,5);check(out.samples==254&&out.discontinuity,"deadline watermark resets with generation");
 GraphExecutor oversized(profile());activate(oversized,g);DeadlineFeed big(g);out={};
 big.send(oversized,out,0,0,8192,2);big.send(oversized,out,1,0,8192,4);
 check(out.samples==8192&&oversized.mixer_input_stats(20,0)->dropped_samples==0&&oversized.mixer_input_stats(20,1)->dropped_samples==0,"matching oversized capture pair survives sequential lane delivery");
 auto comparison=deadline_graph(true);GraphExecutor metric(profile());activate(metric,comparison);DeadlineFeed energy(comparison);out={};out.expected=0;
 energy.send(metric,out,0,0,512,1);energy.send(metric,out,1,0,512,1);check(out.calls==1,"deadline Comparison baseline");
 energy.send(metric,out,0,512,4800,1);energy.send(metric,out,0,5312,1,1);check(out.unavailable==0,"Comparison exact deadline retains current value");
 energy.send(metric,out,0,5313,1,1);check(out.unavailable>0&&out.calls==1,"deadline clears stale Comparison without waiting for gap/overflow");
 std::puts("SourceAligned100ms source-interval deadline/boundary/late/generation/oversized8192/Comparison/noalloc PASS");
}
struct PhysicalObservation {std::size_t fs=0,n=0;unsigned fft=0,mel=0;bool full=false;};
inline void physical_output(void* user,const NumericPortFrameView& frame){auto& out=*static_cast<PhysicalObservation*>(user);if(frame.identity.node!=4&&frame.identity.node!=6)return;
 check(frame.status==Status::ok&&frame.frame.stamp.available,"physical real frame available");const auto& view=std::get<NumericView>(frame.frame.payload);check(validate_view(frame.frame,limits)==Status::ok,"physical real frame validates");
 check(view.contract->clock.sample_rate==out.fs&&view.contract->clock.tick_seconds==Ratio(1,out.fs),"real FFT/Mel sample clock");
 if(frame.identity.node==4){++out.fft;check(view.actual[0]==out.n/2,"real FFT bin count");const auto& axis=view.contract->coordinates[0];check(axis.mode==CoordinateMode::Regular&&axis.unit==NumericUnit::Hertz&&axis.step==Ratio(out.fs,out.n),"real FFT Hz grid Fs/N");
  if(frame.frame.stamp.timestamp_ticks>=std::int64_t(out.n)){float dc=0;check(load_element(view,{0,0,0},dc)==Status::ok&&std::abs(dc-.25f)<1e-5f,"full rectangular FFT DC analytical magnitude");out.full=true;}
 }else{++out.mel;check(view.actual[0]==50&&view.coordinates.axes[0].size()==50*8,"real Mel centres complete");
  for(std::size_t band=0;band<50;++band){std::uint64_t bits=0;for(unsigned byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned>(view.coordinates.axes[0][band*8+byte]))<<(8*byte);
   const double actual=std::bit_cast<double>(bits),expected=700.*std::expm1(std::log1p(double(out.fs)/1400.)*double(band+1)/51.);
   check(std::isfinite(actual)&&std::abs(actual-expected)<1e-8,"real nonlinear Mel centre matches source Fs");}
 }
}
inline void physical_grid_matrix(){for(std::size_t fs:{44100u,48000u,96000u})for(std::size_t n:{512u,4096u}){
 auto g=oracle_graph();geometry(g,fs,n,317);std::erase_if(g.nodes,[](const auto& node){return node.id!=1&&node.id!=2&&node.id!=4&&node.id!=6&&node.id!=10&&node.id!=11;});
 node(g,1).kind="NetworkInput";node(g,1).parameters=EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{fs}}}}};
 node(g,6).inputs[0].contract=node(g,4).outputs[0].contract;
 g.edges={{1,2,0,0},{2,4,0,0},{4,6,0,0},{4,10,0,0},{6,11,0,0}};
 g.configuration_bindings={{4,10,ConfigurationRole::FrequencyGrid,ConfigurationRole::FrequencyGrid},{6,11,ConfigurationRole::FrequencyGrid,ConfigurationRole::FrequencyGrid}};
 check(has_configuration_binding(g,10,ConfigurationRole::FrequencyGrid)&&has_configuration_binding(g,11,ConfigurationRole::FrequencyGrid),"matrix views explicit frequency association");
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"physical matrix Apply");Feed feed(g);PhysicalObservation out{fs,n};
 for(std::size_t sent=0;sent<n+fs/30;){const auto count=std::min<std::size_t>(317,n+fs/30-sent);auto frame=feed.frame(count);feed.samples.fill(.25f);const auto before=allocations;
  check(e.process_numeric_ports(1,frame,{},{physical_output,nullptr,nullptr,&out})==Status::ok&&allocations==before,"physical FFT/Mel matrix noalloc");sent+=count;}
 check(out.fft>0&&out.fft==out.mel&&out.full,"physical matrix real FFT and Mel both observed");
 }
 std::puts("actual FFT/Mel grids Fs44100/48000/96000 x N512/4096 explicit bindings/noalloc PASS");
}
struct StereoObservation {std::array<std::array<std::int64_t,128>,2> ends{};std::array<unsigned,2> count{};unsigned mixed=0;};
inline void stereo_output(void* user,const NumericPortFrameView& frame){auto& out=*static_cast<StereoObservation*>(user);if(frame.identity.node!=104&&frame.identity.node!=404&&frame.identity.node!=4)return;
 check(frame.status==Status::ok&&frame.frame.stamp.available,"stereo numerical real FFT");const auto& view=std::get<NumericView>(frame.frame.payload);float dc=0;check(load_element(view,{0,0,0},dc)==Status::ok,"stereo DC access");
 if(frame.identity.node==4){if(frame.frame.stamp.timestamp_ticks>=4096){check(view.actual[0]==2048&&std::abs(dc-3.f)<1e-5,"mixed PCM4096 rectangular FFT analytical DC3");++out.mixed;}return;}
 const unsigned lane=frame.identity.node==104?0:1;check(out.count[lane]<128&&view.actual[0]==256,"paired FFT512 observation bound");out.ends[lane][out.count[lane]++]=frame.frame.stamp.timestamp_ticks;
 if(frame.frame.stamp.timestamp_ticks>=512)check(std::abs(dc-(lane?4.f:2.f))<1e-5,"separate FFT512 preserves distinct lane DC");
}
inline void stereo_windows_and_mix(){auto g=aligned_fixture(2);
 auto add_branch=[&](std::size_t aperture,NodeId offset,NodeId from,PortId port){auto branch=oracle_graph();geometry(branch,48000,aperture,317);
  for(NodeId id:{2,4}){auto copy=node(branch,id);copy.id+=offset;g.nodes.push_back(std::move(copy));}
  g.edges.push_back({from,2+offset,port,0});g.edges.push_back({2+offset,4+offset,0,0});};
 add_branch(4096,0,20,0);add_branch(512,100,1,0);add_branch(512,400,1,1);
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"paired512 mixed4096 Apply");Feed feed(g);StereoObservation out;
 std::int64_t begin=0;unsigned packet=0;
 while(begin<8192){const auto count=std::min<std::size_t>(packet++%2?193:127,8192-begin);
  for(PortId lane=0;lane<2;++lane){auto frame=feed.frame(count);feed.samples.fill(lane?4.f:2.f);frame.stamp.has_sample_interval=true;frame.stamp.sample_begin=begin;frame.stamp.sample_count=count;frame.stamp.timestamp_ticks=begin+std::int64_t(count)-1;
   const auto before=allocations;check(e.process_numeric_ports(1,lane,frame,{},{stereo_output,nullptr,nullptr,&out})==Status::ok&&allocations==before,"paired512 mixed4096 irregular packets noalloc");}
  begin+=std::int64_t(count);
 }
 check(out.count[0]>0&&out.count[0]==out.count[1]&&out.mixed>0,"both512 and mixed4096 emitted");
 for(unsigned i=0;i<out.count[0];++i)check(out.ends[0][i]==out.ends[1][i],"each paired FFT512 has identical source window end");
 std::puts("stereo distinct DC2/DC4 FFT512 identical ends/mixed4096 analytical DC3/irregular/noalloc PASS");
}
}
