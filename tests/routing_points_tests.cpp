#include <dataflow_runtime/runtime.h>
#include "numeric_test_builders.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace domain;
using namespace signal_contracts;
using namespace dataflow_runtime;
namespace {
std::size_t allocations=0;
void check(bool value,const char* text){if(!value){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
constexpr FrameLimits frames{8,32768,262144,16384,256,8192};
constexpr ProcessingAdmissionLimits limits{32,64,16,16,256,65536,frames};
NumericRuntimeProfile profile(){return {limits,{{4194304,4194304,4194304,131072},0,16777216},{32768,8,262144,262144,262144},{true,true,true}};}
OwnedProcessingContract contract(){
 static constexpr std::array<std::uint32_t,1> mono{0};
 NumericContract c;c.dtype=DType::Float32;c.channel_order=mono;c.clock={"local",{1,48000},48000};
 c.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{8,8},ExtentBounds{}}};
 return numeric_test_builders::contract(c,frames,65536,check,"own fixture contract");
}
ConfiguredProcessingNode make(NodeId id,const char* kind,const OwnedProcessingContract& c){
 ConfiguredProcessingNode n;n.id=id;n.kind=kind;
 if(n.kind!="NetworkInput")n.inputs={{0,PortDirection::Input,c}};
 if(n.kind!="NetworkOutput")n.outputs={{0,PortDirection::Output,c}};
 if(n.kind=="Reroute")n.parameters=RouteProcessingParameters{1,0,true};
 return n;
}
NumericProcessingGraph graph(){auto c=contract();
 return numeric_test_builders::graph("routing-points",{1,120},
  {make(1,"NetworkInput",c),make(2,"Reroute",c),make(3,"Reroute",c),make(4,"NetworkOutput",c),make(5,"NetworkOutput",c)},
  {numeric_test_builders::edge(1,0,2,0),numeric_test_builders::edge(2,0,3,0),
   numeric_test_builders::edge(3,0,4,0),numeric_test_builders::edge(2,0,5,0)});
}
struct Observation {
 const std::byte* storage=nullptr;std::array<unsigned,6> inputs{},outputs{};
 static void accept(void* p,const NumericPortFrameView& f){auto& o=*static_cast<Observation*>(p);
  check(f.status==Status::ok && f.frame.stamp.available,"available frame forwarded");
  const auto& value=std::get<NumericView>(f.frame.payload);check(value.storage.data()==o.storage,"routing does not copy borrowed payload");
  check(f.frame.stamp.sequence==7 && f.frame.stamp.sample_begin==64 && f.frame.stamp.sample_count==8,"routing preserves exact sample interval");
  check(f.identity.node>0 && f.identity.node<6,"original endpoint identity");
  ++(f.direction==PortDirection::Input?o.inputs:o.outputs)[f.identity.node];
 }
};
void runtime(){auto g=graph();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
 check(PreparedGraph::prepare_numeric(g,profile(),plan,state)==Status::ok,"prepare routed graph");
 check(plan->numeric_definition()->nodes.size()==5,"editable graph retains routing topology");
 auto direct=g;std::erase_if(direct.nodes,[](const auto& n){return n.kind=="Reroute";});direct.edges={{1,4,0,0},{1,5,0,0}};
 std::shared_ptr<const PreparedGraph> straight;std::unique_ptr<RuntimeContext> straight_state;
 check(PreparedGraph::prepare_numeric(direct,profile(),straight,straight_state)==Status::ok,"prepare equivalent direct fanout");
 check(plan->memory_requirements().buffer_bytes==straight->memory_requirements().buffer_bytes && plan->memory_requirements().scratch_bytes==straight->memory_requirements().scratch_bytes,"routing aliases add no payload or scratch buffers");
 check(!plan->node_stats(2,*state) && !plan->node_stats(3,*state),"routing points have no executor state or DSP statistics");
 check(plan->port_contract(2,PortDirection::Input,0).has_value() && plan->port_contract(3,PortDirection::Output,0).has_value(),"routing endpoint contracts remain queryable");
 DataFrameView snapshot;check(plan->history(3,0,*state,snapshot)==Status::incompatible_contract,"numeric query can fall back to routing observer cache");
 std::array<float,8> data{0,1,-2,3,-4,5,-6,7};const auto owned=contract();const auto c=std::get<NumericContract>(owned.view());
 const PlaneLayout plane{0,{sizeof(float),0}};FrameStamp stamp;stamp.sequence=7;stamp.producer_epoch=1;stamp.sample_begin=64;stamp.sample_count=8;stamp.has_sample_interval=true;
 auto value=numeric_test_builders::frame(stamp,c,std::as_bytes(std::span(data)),std::span(&plane,1),{8,1});
 Observation observed;observed.storage=std::get<NumericView>(value.payload).storage.data();const auto before=allocations;
 check(plan->process_numeric_ports(1,value,*state,{}, {Observation::accept,Observation::accept,nullptr,&observed})==Status::ok,"route ingress");
 check(allocations==before,"routing hot path has no allocations");
 check(observed.inputs[2]==1 && observed.outputs[2]==1 && observed.inputs[3]==1 && observed.outputs[3]==1,"routing endpoints remain observable");
 check(observed.inputs[4]==1 && observed.inputs[5]==1,"fanout reaches both consumers exactly once");
}
void lowering(){auto g=graph();NumericProcessingGraph out;std::vector<RoutingAlias> aliases;
 check(bool(lower_routing_points(g,limits,out,aliases)),"lower nested routing");
 check(out.nodes.size()==3 && out.edges.size()==2 && aliases.size()==2,"compiler removes only routing nodes");
 check(out.edges[0].from==1 && out.edges[0].to==4 && out.edges[1].from==1 && out.edges[1].to==5,"compiler builds direct fanout");
 auto before=out;auto cyclic=g;cyclic.edges[0]={3,2,0,0};
 check(!lower_routing_points(cyclic,limits,out,aliases),"routing-only cycle rejected");
 check(out.edges==before.edges && aliases.size()==2,"failed lowering publishes nothing");
 auto multiple=g;multiple.edges.push_back({1,2,0,0});check(!lower_routing_points(multiple,limits,out,aliases),"two producers at routing input rejected");
 auto bad=g;bad.edges[1].output_port=1;check(!lower_routing_points(bad,limits,out,aliases),"invalid routing output rejected before elimination");
 auto duplicate=g;duplicate.nodes.push_back(g.nodes[1]);check(!lower_routing_points(duplicate,limits,out,aliases),"duplicate hidden node ID rejected");
 auto disconnected=g;disconnected.edges.erase(disconnected.edges.begin());check(bool(lower_routing_points(disconnected,limits,out,aliases)),"disconnected routing lowers to disconnected input");check(out.edges.empty(),"disconnected route does not retain stale source");
 auto converted=g;auto wide=std::get<NumericContract>(converted.nodes[0].outputs[0].contract.view());wide.dtype=DType::Float64;
 check(own_processing_contract(wide,frames,65536,converted.nodes[0].outputs[0].contract)==Status::ok,"own conversion fixture");
 NumericProcessingGraph typed;check(bool(lower_dtype_edges(converted,limits,typed)),"materialize dtype conversion before reroute");
 check(bool(lower_routing_points(typed,limits,out,aliases)) && out.nodes.size()==4 && aliases[0].source!=1,"routing preserves intermediate conversion rather than bypassing it");
}
struct HistoryObservation {
 unsigned rows=0;
 static void row(void* p,const NumericHistoryRowView& event){auto& o=*static_cast<HistoryObservation*>(p);
  if(event.endpoint.identity.node!=3 || event.endpoint.direction!=PortDirection::Output || event.event!=NumericHistoryEventKind::Row)return;
  ++o.rows;check(event.accepted_row_ordinal==o.rows && event.filled_count==o.rows && event.capacity==3,"routing history aliases preserve row ordinals and occupancy");
 }
};
void history(){
 const auto input_owner=contract();const auto input=std::get<NumericContract>(input_owner.view());
 const auto own=[](const NumericContract& c){return numeric_test_builders::contract(c,frames,65536,check,"own history contract");};
 auto matrix=input;matrix.semantic=Semantic::History;matrix.history_element=input.semantic;
 matrix.shape={Rank::Matrix,{Axis::Time,Axis::Sample},{ExtentBounds{0,3},ExtentBounds{8,8}}};
 matrix.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}};
 NumericContract count;count.dtype=DType::UInt64;count.semantic=Semantic::Scalar;count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;count.channel_order=input.channel_order;count.clock=input.clock;
 ConfiguredProcessingNode store;store.id=2;store.kind="Spectrogram";store.parameters=HistoryProcessingParameters{3};store.inputs={{0,PortDirection::Input,input_owner}};
 const auto matrix_owner=own(matrix);store.outputs={{0,PortDirection::Output,matrix_owner},{1,PortDirection::Output,own(count)}};
 auto g=numeric_test_builders::graph("routing-history",{1,120},
  {make(1,"NetworkInput",input_owner),store,make(3,"Reroute",matrix_owner),make(4,"NetworkOutput",matrix_owner)},
  {numeric_test_builders::edge(1,0,2,0),numeric_test_builders::edge(2,0,3,0),numeric_test_builders::edge(3,0,4,0)});
 std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;check(PreparedGraph::prepare_numeric(g,profile(),plan,state)==Status::ok,"prepare routed history");
 HistoryObservation o;std::array<float,8> samples{};const PlaneLayout plane{0,{sizeof(float),0}};
 for(unsigned i=0;i<3;++i){samples.fill(float(i+1));FrameStamp stamp;stamp.producer_epoch=1;stamp.sequence=i+1;stamp.sample_begin=i*8;stamp.sample_count=8;stamp.timestamp_ticks=i*8+7;stamp.has_sample_interval=true;
  auto v=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(samples)),std::span(&plane,1),{8,1});const auto before=allocations;
  check(plan->process_numeric_ports(1,v,*state,{}, {nullptr,nullptr,HistoryObservation::row,&o})==Status::ok && allocations==before,"routed history callback remains allocation-free");
 }
 check(o.rows==3,"routing output receives every compact history row");DataFrameView snapshot;std::uint64_t filled=0,ordinal=0;
 check(plan->history(3,0,*state,snapshot,&filled,&ordinal)==Status::ok && filled==3 && ordinal==3,"history snapshot resolves through compiled alias");
 for(unsigned i=0;i<3;++i){float value=0;check(load_element(std::get<NumericView>(snapshot.payload),{0,i,0},value)==Status::ok && value==float(i+1),"routing retains chronological history values");}
}
}
void* operator new(std::size_t n){++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main(){lowering();runtime();history();std::puts("transparent routing compiler / aliases / borrowed fanout / history / cycle / atomicity PASS");}
