#pragma once
#include <core_app/numeric_default_graph.h>
#include <dataflow_runtime/runtime.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
namespace canonical_test {
using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
inline std::size_t allocations=0;
inline void check(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
inline constexpr FrameLimits limits{8,32768,262144,16384,256,8192};
// Retained numerical oracles below explicitly use 60 Hz; the product default remains 120 Hz.
inline NumericProcessingGraph oracle_graph(){auto g=numeric_test_graph();g.execution_interval={1,60};return g;}
// Two branches plus six auxiliary constants fit40nodes. Converter support
// adds its8192-byte reference stack reservation to the65536-byte graph budget.
inline NumericRuntimeProfile profile(){return {{40,64,8,16,256,65536,limits},{{1048576,4*1048576,1048576,73728},0,8388608},{32768,8,262144,262144,262144},{true,true,true}};}
inline ConfiguredProcessingNode& node(NumericProcessingGraph& g,NodeId id){auto it=std::find_if(g.nodes.begin(),g.nodes.end(),[&](const auto& n){return n.id==id;});check(it!=g.nodes.end(),"missing node");return *it;}
inline NumericContract adjusted_geometry(NumericContract contract,std::size_t rate,std::size_t window,std::size_t block){
 contract.clock={"local",{1,std::uint64_t(rate)},std::uint32_t(rate)};
 if(contract.semantic==Semantic::PCM)contract.shape.extents[0]={0,block};
 if(contract.semantic==Semantic::Window || contract.semantic==Semantic::Windowed)contract.shape.extents[0]={window,window};
 if(contract.semantic==Semantic::SpectrumMagnitude){contract.shape.extents[0]={window/2,window/2};contract.coordinates[0].step={std::int64_t(rate),std::uint64_t(window)};}
 return contract;
}
inline void source_geometry(ConfiguredProcessingNode& source_node,std::size_t rate){
 auto& source=std::get<EndpointProcessingParameters>(source_node.parameters).values;bool has_rate=false;
 for(auto& value:source){
  if(value.key=="sample_rate"){value.value=NumericLiteral{std::uint64_t(rate)};has_rate=true;}
  if(value.key=="maximum_hz")value.value=NumericLiteral{double(rate)*.3};
 }
 if(!has_rate)source.push_back({"sample_rate",NumericLiteral{std::uint64_t(rate)}});
}
inline void geometry(NumericProcessingGraph& graph,std::size_t rate,std::size_t window,std::size_t block){
 for(auto& configured:graph.nodes)for(auto* ports:{&configured.inputs,&configured.outputs})for(auto& port:*ports){
  const auto contract=adjusted_geometry(std::get<NumericContract>(port.contract.view()),rate,window,block);
  check(own_processing_contract(contract,limits,65536,port.contract)==Status::ok,"own adjusted contract");
 }
 for(auto& configured:graph.nodes){
  if(configured.kind!="Window" && configured.kind!="BarkWeighting" && configured.kind!="Mel")continue;
  auto& values=std::get<TransformProcessingParameters>(configured.parameters).values;
  for(auto& field:values){
   if(field.key=="size" && configured.kind=="Window")field.value=NumericLiteral{std::uint64_t(window)};
   if(field.key=="input_rate" && configured.kind=="Window")field.value=NumericLiteral{std::uint64_t(rate)};
   if(field.key=="sample_rate" && configured.kind!="Window")field.value=NumericLiteral{std::uint64_t(rate)};
   if(field.key=="bin_hz" && configured.kind!="Window")field.value=NumericLiteral{double(rate)/double(window)};
  }
 }
 source_geometry(node(graph,1),rate);
}
struct Feed {
 NumericContract contract;std::array<float,317> samples{};std::array<PlaneLayout,1> planes{{{0,{4,0}}}};std::uint64_t sent=0,sequence=0,epoch=17;
 explicit Feed(const NumericProcessingGraph& g):contract(std::get<NumericContract>(g.nodes[0].outputs[0].contract.view())){}
 float sample(std::size_t index) const {
  double value=.15*std::sin(6.283185307179586*(sent+index)*512/contract.clock.sample_rate);
  for(std::size_t tone=0;tone<6;++tone){
   const auto hz=64*(tone+1);const auto phase=6.283185307179586*(sent+index)*hz/contract.clock.sample_rate;
   const auto amplitude=.003+.001*std::sin(double(sequence)*.07+tone);value+=amplitude*std::sin(phase);
  }
  return float(value);
 }
 FrameStamp next_stamp(std::size_t count){
  FrameStamp stamp;stamp.sequence=++sequence;stamp.source_sequence=sequence;stamp.producer_epoch=epoch;stamp.generation=4;
  stamp.timestamp_ticks=std::int64_t(sent);stamp.stream_id="canonical.audit";sent+=count;return stamp;
 }
 DataFrameView frame(std::size_t count){
  for(std::size_t index=0;index<count;++index)samples[index]=sample(index);
  const auto stamp=next_stamp(count);
  return {stamp,NumericView{&contract,std::as_bytes(std::span(samples)),planes,{count,1},{},{}}};
 }
 void process(GraphExecutor& e,std::size_t count,NumericRuntimeCallbacks callbacks={}){auto f=frame(count);const auto before=allocations;check(e.process_numeric_ports(1,f,{1,1},callbacks)==Status::ok,"source process");check(allocations==before,"hot path allocation");}
};
inline void print_contract(const NumericContract& contract){
 std::fprintf(stderr,"semantic=%d dtype=%d norm=%d clock=%.*s %lld/%llu rate=%u channels=",int(contract.semantic),int(contract.dtype),int(contract.representation.normalization),int(contract.clock.domain.size()),contract.clock.domain.data(),(long long)contract.clock.tick_seconds.numerator,(unsigned long long)contract.clock.tick_seconds.denominator,contract.clock.sample_rate);
 for(auto channel:contract.channel_order)std::fprintf(stderr,"%u,",channel);
 std::fprintf(stderr,"\n");
}
inline void print_invalid_node(const ConfiguredProcessingNode& configured,const ProcessingAdmissionLimits& admission){
 ResolvedProcessingNode resolved;const auto diagnostic=resolve_processing_node(configured,admission,resolved);
 if(diagnostic)return;
 std::fprintf(stderr,"node=%d kind=%s error=%d port=%u\n",configured.id,configured.kind.c_str(),int(diagnostic.error),unsigned(diagnostic.port));
 for(auto* ports:{&configured.inputs,&configured.outputs})for(const auto& port:*ports){
  const auto contract=port.contract.view();
  if(const auto* numeric=std::get_if<NumericContract>(&contract))print_contract(*numeric);
 }
}
inline void prepare(GraphExecutor& executor,const NumericProcessingGraph& graph,application::PreparedExecution& output){
 const auto runtime_profile=profile();const auto status=executor.prepare_numeric(graph,{runtime_profile.admission,runtime_profile.resources},output);
 if(status!=Status::ok){
  std::fprintf(stderr,"prepare status=%d nodes=%zu edges=%zu\n",int(status),graph.nodes.size(),graph.edges.size());
  for(const auto& configured:graph.nodes)print_invalid_node(configured,runtime_profile.admission);
 }
 check(status==Status::ok&&bool(output),"prepare");
}
inline Status restart(GraphExecutor& e,const NumericProcessingGraph& g){application::PreparedExecution pending;prepare(e,g,pending);return e.apply(std::move(pending));}
inline std::size_t rows(const GraphExecutor& e,NodeId id=7){DataFrameView f;std::uint64_t filled=0;check(e.plan()->history(id,0,*e.context(),f,&filled)==Status::ok,"history query");return std::size_t(filled);}
}
void* operator new(std::size_t n){++canonical_test::allocations;auto* p=std::malloc(n?n:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}



