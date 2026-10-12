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
inline NumericRuntimeProfile profile(){return {{32,64,8,16,256,65536,limits},{{1048576,1048576,1048576,65536},0,8388608},{32768,8,262144,262144,262144},{true,true,true}};}
inline ConfiguredProcessingNode& node(NumericProcessingGraph& g,NodeId id){auto it=std::find_if(g.nodes.begin(),g.nodes.end(),[&](const auto& n){return n.id==id;});check(it!=g.nodes.end(),"missing node");return *it;}
inline void geometry(NumericProcessingGraph& g,std::size_t rate,std::size_t window,std::size_t block){
 for(auto& n:g.nodes)for(auto* ports:{&n.inputs,&n.outputs})for(auto& port:*ports){auto c=std::get<NumericContract>(port.contract.view());c.clock={"local",{1,std::uint64_t(rate)},std::uint32_t(rate)};
  if(c.semantic==Semantic::PCM)c.shape.extents[0]={0,block};
  if(c.semantic==Semantic::Window||c.semantic==Semantic::Windowed)c.shape.extents[0]={window,window};
  if(c.semantic==Semantic::SpectrumMagnitude){c.shape.extents[0]={window/2,window/2};c.coordinates[0].step={std::int64_t(rate),std::uint64_t(window)};}
  check(own_processing_contract(c,limits,65536,port.contract)==Status::ok,"own adjusted contract");}
 auto& values=std::get<TransformProcessingParameters>(node(g,2).parameters).values;values[0].value=NumericLiteral{std::uint64_t(window)};
 auto& source=std::get<EndpointProcessingParameters>(node(g,1).parameters).values;bool has_rate=false;
 for(auto& v:source){if(v.key=="sample_rate"){v.value=NumericLiteral{std::uint64_t(rate)};has_rate=true;}if(v.key=="maximum_hz")v.value=NumericLiteral{double(rate)*.3};}
 if(!has_rate)source.push_back({"sample_rate",NumericLiteral{std::uint64_t(rate)}});
}
struct Feed {
 NumericContract contract;std::array<float,317> samples{};std::array<PlaneLayout,1> planes{{{0,{4,0}}}};std::uint64_t sent=0,sequence=0,epoch=17;
 explicit Feed(const NumericProcessingGraph& g):contract(std::get<NumericContract>(g.nodes[0].outputs[0].contract.view())){}
 DataFrameView frame(std::size_t count){
  for(std::size_t i=0;i<count;++i){double left=.15*std::sin(6.283185307179586*(sent+i)*512/contract.clock.sample_rate);
   for(std::size_t tone=0;tone<6;++tone){const auto hz=64*(tone+1);const auto phase=6.283185307179586*(sent+i)*hz/contract.clock.sample_rate;const auto amp=.003+.001*std::sin(double(sequence)*.07+tone);left+=amp*std::sin(phase);}
   samples[i]=float(left);}
  FrameStamp stamp;stamp.sequence=++sequence;stamp.source_sequence=sequence;stamp.producer_epoch=epoch;stamp.generation=4;stamp.timestamp_ticks=std::int64_t(sent);stamp.stream_id="canonical.audit";sent+=count;
  return {stamp,NumericView{&contract,std::as_bytes(std::span(samples)),planes,{count,1},{},{}}};
 }
 void process(GraphExecutor& e,std::size_t count,NumericRuntimeCallbacks callbacks={}){auto f=frame(count);const auto before=allocations;check(e.process_numeric_ports(1,f,{1,1},callbacks)==Status::ok,"source process");check(allocations==before,"hot path allocation");}
};
inline void prepare(GraphExecutor& e,const NumericProcessingGraph& g,application::PreparedExecution& out){auto p=profile();const auto status=e.prepare_numeric(g,{p.admission,p.resources},out);if(status!=Status::ok){std::fprintf(stderr,"prepare status=%d nodes=%zu edges=%zu\n",int(status),g.nodes.size(),g.edges.size());for(const auto& n:g.nodes){ResolvedProcessingNode resolved;const auto d=resolve_processing_node(n,p.admission,resolved);if(!d){std::fprintf(stderr,"node=%d kind=%s error=%d port=%u\n",n.id,n.kind.c_str(),int(d.error),unsigned(d.port));for(auto* ports:{&n.inputs,&n.outputs})for(const auto& port:*ports){auto c=std::get<NumericContract>(port.contract.view());std::fprintf(stderr,"semantic=%d dtype=%d norm=%d clock=%.*s %lld/%llu rate=%u channels=",int(c.semantic),int(c.dtype),int(c.representation.normalization),int(c.clock.domain.size()),c.clock.domain.data(),(long long)c.clock.tick_seconds.numerator,(unsigned long long)c.clock.tick_seconds.denominator,c.clock.sample_rate);for(auto x:c.channel_order)std::fprintf(stderr,"%u,",x);std::fprintf(stderr,"\n");}}}}check(status==Status::ok&&bool(out),"prepare");}
inline Status restart(GraphExecutor& e,const NumericProcessingGraph& g){application::PreparedExecution pending;prepare(e,g,pending);return e.apply(std::move(pending));}
inline std::size_t rows(const GraphExecutor& e,NodeId id=7){DataFrameView f;std::uint64_t filled=0;check(e.plan()->history(id,0,*e.context(),f,&filled)==Status::ok,"history query");return std::size_t(filled);}
}
void* operator new(std::size_t n){++canonical_test::allocations;auto* p=std::malloc(n?n:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}



