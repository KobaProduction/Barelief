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
static void measure(std::uint64_t capacity,bool subscribe){
 auto defaults=numeric_default_graph();auto history=*std::find_if(defaults.nodes.begin(),defaults.nodes.end(),[](const auto& n){return n.id==7;}),sink=*std::find_if(defaults.nodes.begin(),defaults.nodes.end(),[](const auto& n){return n.id==8;});
 auto c=std::get<NumericContract>(history.inputs[0].contract.view());
 auto hc=std::get<NumericContract>(history.outputs[0].contract.view());hc.shape.extents[0].maximum=capacity;
 domain::OwnedProcessingContract owner;require(domain::own_processing_contract(hc,bdsp_protocol::numeric_control::desktop_limits().frame,16384,owner)==Status::ok,"own History geometry");
 history.outputs[0].contract=owner;sink.inputs[0].contract=owner;history.parameters=domain::HistoryProcessingParameters{capacity};
 domain::ConfiguredProcessingNode input;input.id=1;input.kind="NetworkInput";input.outputs.push_back({0,domain::PortDirection::Output,history.inputs[0].contract});
 domain::NumericProcessingGraph graph;graph.graph_id="main";graph.nodes={input,history,sink};graph.edges={{1,7,0,0},{7,8,0,0}};
 NumericEngine engine;Replies replies(engine);replies.accepted(apply(graph));
 const EndpointIdentity output{"main",8,domain::PortDirection::Input,0},source{"main",1,domain::PortDirection::Output,0};
 if(subscribe)require(engine.subscribe_numeric_frames({source}),"selected source subscription");
 std::array<std::byte,50*8> frequencyBytes{};
 for(std::size_t band=0;band<50;++band){const auto bits=std::bit_cast<std::uint64_t>(double(band+1)*100.);for(std::size_t byte=0;byte<8;++byte)frequencyBytes[band*8+byte]=std::byte((bits>>(byte*8))&255);}
 std::array<float,50> values{};PlaneLayout plane{0,{4,0}};FrameStamp stamp;stamp.epoch=700;stamp.graph_revision=800;stamp.producer_epoch=701;stamp.generation=9;
 auto process=[&](std::uint64_t sequence){stamp.sequence=stamp.source_sequence=sequence;stamp.timestamp_ticks=std::int64_t(sequence);values.fill(float(sequence));require(engine.feed_numeric(1,{stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{50,1},{},CoordinateView{{std::span<const std::byte>(frequencyBytes),{}}}}})==Status::ok,"canonical Mel ingress");};
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
 values.fill(-1);require(load_element(view,{0,capacity-1,0},last)==Status::ok&&last==412,"History retained borrowed ingress memory");
 std::printf("capacity=%llu subscribed=%d hot_allocations=%zu hot_bytes=%zu ordinal=%llu owned_snapshot_preserved\n",(unsigned long long)capacity,int(subscribe),allocations,allocation_bytes,(unsigned long long)ordinal);
}
int main(int argc,char**argv){QCoreApplication app(argc,argv);try{measure(8,false);measure(400,false);measure(8,true);measure(400,true);return 0;}catch(const std::exception&e){measuring=false;std::fprintf(stderr,"%s\n",e.what());return 1;}}
