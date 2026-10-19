#include <algorithm>
#include "numeric_transport_fixture.h"
#include <core_app/numeric_default_graph.h>
#include <bit>
#include <array>
#include <cstdlib>
#include <new>
#include <cstdio>
using namespace transport_fixture;
static thread_local bool measuring=false;
static thread_local std::size_t allocations=0,allocation_bytes=0;
void* operator new(std::size_t size){if(measuring){++allocations;allocation_bytes+=size;}if(auto*p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void*p)noexcept{std::free(p);}void operator delete[](void*p)noexcept{std::free(p);}
void operator delete(void*p,std::size_t)noexcept{std::free(p);}void operator delete[](void*p,std::size_t)noexcept{std::free(p);}
static void measure(std::uint64_t capacity,bool subscribe,std::size_t columns=50){
 auto defaults=numeric_default_graph();auto history=*std::find_if(defaults.nodes.begin(),defaults.nodes.end(),[](const auto& n){return n.id==7;}),sink=*std::find_if(defaults.nodes.begin(),defaults.nodes.end(),[](const auto& n){return n.id==8;});
 auto c=std::get<NumericContract>(history.inputs[0].contract.view());c.shape.extents[0]={columns,columns};
 auto hc=std::get<NumericContract>(history.outputs[0].contract.view());hc.shape.extents[0].maximum=capacity;hc.shape.extents[1]={columns,columns};
 constexpr FrameLimits geometryLimits{8,131072,524288,16384,256,8192};
 domain::OwnedProcessingContract owner;require(domain::own_processing_contract(hc,geometryLimits,16384,owner)==Status::ok,"own History geometry");
 require(domain::own_processing_contract(c,geometryLimits,16384,history.inputs[0].contract)==Status::ok,"own History row geometry");
 history.outputs[0].contract=owner;sink.inputs[0].contract=owner;history.parameters=domain::HistoryProcessingParameters{capacity};
 domain::ConfiguredProcessingNode input;input.id=1;input.kind="NetworkInput";input.outputs.push_back({0,domain::PortDirection::Output,history.inputs[0].contract});
 domain::NumericProcessingGraph graph;graph.graph_id="main";graph.nodes={input,history,sink};graph.edges={{1,7,0,0},{7,8,0,0}};
 NumericEngine engine;Replies replies(engine);replies.accepted(apply(graph));
 const EndpointIdentity output{"main",8,domain::PortDirection::Input,0},source{"main",1,domain::PortDirection::Output,0};
 if(subscribe)require(engine.subscribe_numeric_frames({source}),"selected source subscription");
 std::vector<std::byte> frequencyBytes(columns*8);
 for(std::size_t band=0;band<columns;++band){const auto bits=std::bit_cast<std::uint64_t>(double(band+1)*100.);for(std::size_t byte=0;byte<8;++byte)frequencyBytes[band*8+byte]=std::byte((bits>>(byte*8))&255);}
 std::vector<float> values(columns);PlaneLayout plane{0,{4,0}};FrameStamp stamp;stamp.epoch=700;stamp.graph_revision=800;stamp.producer_epoch=701;stamp.generation=9;
 auto process=[&](std::uint64_t sequence){stamp.sequence=stamp.source_sequence=sequence;stamp.timestamp_ticks=std::int64_t(sequence);std::fill(values.begin(),values.end(),float(sequence));require(engine.feed_numeric(1,{stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{columns,1},{},CoordinateView{{std::span<const std::byte>(frequencyBytes),{}}}}})==Status::ok,"canonical Mel ingress");};
 for(std::uint64_t sequence=1;sequence<=400;++sequence){process(sequence);if(sequence%32==0)QCoreApplication::processEvents();}
 auto retained=query(engine,output);const auto old=std::get<NumericView>(retained.frame.view().payload);float oldest=0;require(load_element(old,{0,0,0},oldest)==Status::ok,"retained oldest row");
 const auto oldOrdinal=std::uint64_t{400};std::uint64_t ordinal=0;
 allocations=allocation_bytes=0;measuring=true;for(std::uint64_t sequence=401;sequence<=412;++sequence)process(sequence);measuring=false;
 require(allocations==0,"adapter processing allocated/copied growing History");
 auto current=query(engine,output);require(engine.query_numeric(output,current,&ordinal)==Status::ok&&ordinal>oldOrdinal,"unsubscribed History stopped");
 const auto view=std::get<NumericView>(current.frame.view().payload);float last=0,oldAgain=0;
 require(view.actual[0]==capacity&&load_element(view,{0,capacity-1,0},last)==Status::ok&&last==412,"cold latest state incorrect");
 require(load_element(std::get<NumericView>(retained.frame.view().payload),{0,0,0},oldAgain)==Status::ok&&oldAgain==oldest,"cold owned snapshot changed after ingress");
 const auto state=engine.snapshot();const auto observedStamp=current.frame.view().stamp;
 require(observedStamp.producer_epoch==701&&observedStamp.generation==9&&observedStamp.epoch==state.epoch&&observedStamp.graph_revision==state.graph_revision&&current.delivery.epoch==state.epoch&&current.delivery.graph_revision==state.graph_revision,"cold History producer provenance or service delivery identity changed");
 std::fill(values.begin(),values.end(),-1);require(load_element(view,{0,capacity-1,0},last)==Status::ok&&last==412,"History retained borrowed ingress memory");
 require(view.actual[1]==columns,"History snapshot columns changed");
 if(columns==220)require(view.storage.size()>262144,"wide History did not cross old 256KiB snapshot limit");
 std::printf("capacity=%llu columns=%zu subscribed=%d hot_allocations=%zu hot_bytes=%zu ordinal=%llu owned_snapshot_preserved\n",(unsigned long long)capacity,columns,int(subscribe),allocations,allocation_bytes,(unsigned long long)ordinal);
}
static void large_snapshot_lifetime_and_bound(){
 constexpr std::size_t rows=400,columns=220;constexpr FrameLimits limits{8,131072,524288,16384,256,8192};
 DeliveredNumericFrame survivor;std::weak_ptr<const void> retainedOwner;
 {
  NumericContract input=contract(columns);input.dtype=DType::Float32;input.semantic=Semantic::Numeric;input.representation.alignment=alignof(float);input.shape.axes[0]=Axis::Band;
  auto historyContract=input;historyContract.semantic=Semantic::History;historyContract.history_element=input.semantic;
  historyContract.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{0,rows},ExtentBounds{columns,columns}}};
  historyContract.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}};
  auto own=[&](const NumericContract& value){domain::OwnedProcessingContract owned;require(domain::own_processing_contract(value,limits,16384,owned)==Status::ok,"own large cold History contract");return owned;};
  domain::ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={{0,domain::PortDirection::Output,own(input)}};
  domain::ConfiguredProcessingNode history;history.id=7;history.kind="Spectrogram";history.parameters=domain::HistoryProcessingParameters{rows};history.inputs={{0,domain::PortDirection::Input,own(input)}};history.outputs={{0,domain::PortDirection::Output,own(historyContract)}};
  auto count=input;count.dtype=DType::UInt64;count.semantic=Semantic::Scalar;count.shape={};count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;count.coordinates={};count.representation.alignment=alignof(std::uint64_t);
  history.outputs.push_back({domain::filled_count_output,domain::PortDirection::Output,own(count)});
  domain::NumericProcessingGraph graph;graph.graph_id="large-history-leases";graph.nodes={source,history};graph.edges={{1,7,0,0}};
  NumericEngine engine;Replies replies(engine);replies.accepted(apply(graph));
  std::array<float,columns> values{};values.fill(.125f);values.back()=.75f;const PlaneLayout plane{0,{sizeof(float),0}};
  FrameStamp stamp;stamp.sequence=stamp.source_sequence=1;stamp.producer_epoch=19;stamp.generation=1;stamp.timestamp_ticks=1;
  require(engine.feed_numeric(1,{stamp,NumericView{&input,std::as_bytes(std::span(values)),std::span(&plane,1),{columns,1},{},{}}})==Status::ok,"feed large cold History");
  const EndpointIdentity endpoint{graph.graph_id,7,domain::PortDirection::Output,0};std::array<DeliveredNumericFrame,8> snapshots;
  for(auto& snapshot:snapshots){require(engine.query_numeric(endpoint,snapshot)==Status::ok,"all eight cold snapshot leases available");const auto view=std::get<NumericView>(snapshot.frame.view().payload);require(view.storage.size()==rows*columns*sizeof(float),"full large History bytes preserved");}
  auto rejected=snapshots[0];const auto oldData=std::get<NumericView>(rejected.frame.view().payload).storage.data();
  require(engine.query_numeric(endpoint,rejected)==Status::exhausted,"ninth cold snapshot bounded instead of fallback allocation");
  require(std::get<NumericView>(rejected.frame.view().payload).storage.data()==oldData,"exhausted query does not replace caller-owned output");
  snapshots[3]={};require(engine.query_numeric(endpoint,snapshots[3])==Status::ok,"released cold snapshot slot reusable");
  survivor=snapshots[3];retainedOwner=survivor.context->retention_owner;require(!retainedOwner.expired(),"snapshot pins owning arena");
 }
 require(!retainedOwner.expired(),"cold snapshot arena survives NumericEngine teardown");
 const auto owned=std::get<NumericView>(survivor.frame.view().payload);float first=0,last=0;
 require(owned.actual==std::array<std::size_t,2>{rows,columns} && load_element(owned,{0,rows-1,0},first)==Status::ok && first==.125f && load_element(owned,{0,rows-1,columns-1},last)==Status::ok && last==.75f,"full retained History remains readable after engine destruction");
 survivor={};require(retainedOwner.expired(),"last snapshot release frees detached arena ownership");
 std::puts("large History snapshot: 352000 bytes, eight bounded leases, exhaustion, reuse, retained teardown PASS");
}
int main(int argc,char**argv){QCoreApplication app(argc,argv);try{measure(8,false);measure(400,false);measure(8,true);measure(400,true);measure(400,false,220);measure(400,true,220);large_snapshot_lifetime_and_bound();return 0;}catch(const std::exception&e){measuring=false;std::fprintf(stderr,"%s\n",e.what());return 1;}}
