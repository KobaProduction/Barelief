#include <dataflow_runtime/runtime.h>
#include <device_protocols/wled_packet.h>
#include <wled_protocol/wled_packet.h>
#include <signal_contracts/buffer_pool.h>
#include <bdsp_protocol/stream_codec.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <new>
#include <limits>
#include <iostream>
static bool measuring=false;
static std::size_t allocations=0;
void *operator new(std::size_t size) {if(measuring)++allocations;if(auto *p=std::malloc(size?size:1))return p;std::abort();}
void operator delete(void *p) noexcept {std::free(p);}
void operator delete(void *p,std::size_t) noexcept {std::free(p);}
using namespace domain;
using namespace dataflow_runtime;
using namespace signal_contracts;
static NodeDefinition node(int id,NodeKind k) {return {id,k,default_parameters(k)};}
struct Observation {std::array<float,20> value{};std::array<std::size_t,20> rows{},calls{};device_protocols::WledPacket packet{};};
static void observe(void *user,const OutputView &out) {
    auto &o=*static_cast<Observation *>(user);
    assert(validate(out.frame)==Status::ok || validate(out.frame)==Status::unavailable);
    if(out.output_port)return;
    ++o.calls[out.node];
    o.value[out.node]=out.frame.values.empty()?0:out.frame.values[0];
    if(out.frame.history.frames) {o.rows[out.node]=out.frame.history.frames;o.value[out.node]=out.frame.history.row(out.frame.history.frames-1)[0];}
    if(!out.frame.packet.empty())std::copy(out.frame.packet.begin(),out.frame.packet.end(),o.packet.begin());
}
struct PortObservation {
    std::array<std::array<std::size_t,8>,20> output{},input{};
    std::array<FrameType,2> history_types{};
    bool unavailable=false;
};
static void observe_output(void *user,const PortFrameView &out) {
    auto &o=*static_cast<PortObservation*>(user);
    assert(out.direction==PortDirection::Output && out.graph_id=="routing-graph");
    assert(out.delivery.graph_revision==17 && out.delivery.epoch==29);
    assert(out.frame.metadata.graph_revision==101 && out.frame.metadata.epoch==103);
    assert(out.frame.metadata.producer_epoch==0);
    assert(out.contract_known);
    ++o.output[out.identity.node][out.identity.port];
    if(out.identity.node==3)o.history_types[out.identity.port]=out.frame.type;
    if(o.unavailable)assert(!out.frame.metadata.available && out.frame.metadata.sequence==0 && out.frame.metadata.discontinuity);
}
static void observe_input(void *user,const PortFrameView &in) {
    auto &o=*static_cast<PortObservation*>(user);
    assert(in.direction==PortDirection::Input && input_ports(node(in.identity.node,in.kind)));
    ++o.input[in.identity.node][in.identity.port];
}
static void retention_test() {
    alignas(float) std::array<std::byte,256> storage{};
    std::array<BufferSlot,2> slots{};
    std::array<std::byte,64> identity_storage{};std::array<BufferSlot,2> identity_slots{};
    BufferPool payloads(storage,slots,128),identities(identity_storage,identity_slots,32);
    RetainedPortFrame first,second;
    {
        std::string graph="temporary-graph",stream="temporary-stream";
        std::array<float,2> values{2,4};
        FrameView f;f.type=FrameType::Mel;f.values=values;f.metadata.stream_id=stream;
        f.metadata.sample_rate=48000;f.metadata.bands=2;f.metadata.sequence=1;
        PortFrameView delivery{graph,{3,0},PortDirection::Output,NodeKind::Mel,contract(f),true,f,false,{7,9}};
        assert(retain_port_frame(delivery,payloads,identities,first)==Status::ok);
        delivery.identity.port=1;
        assert(retain_port_frame(delivery,payloads,identities,second)==Status::ok);
        values.fill(99);graph.assign(graph.size(),'x');stream.assign(stream.size(),'x');
        const auto old=first.view();
        assert(retain_port_frame(delivery,payloads,identities,first)==Status::exhausted);
        assert(first.view().identity.port==old.identity.port && first.view().frame.values[0]==2);
    }
    auto copy=first;first={};
    assert(copy.view().graph_id=="temporary-graph" && copy.view().frame.metadata.stream_id=="temporary-stream");
    assert(copy.view().delivery.epoch==9 && second.view().identity.port==1);
    copy={};second={};assert(!payloads.in_use() && !identities.in_use());
    // Identity acquire failure releases newly acquired payload and preserves out.
    std::array<std::byte,1> tiny{};std::array<BufferSlot,1> tiny_slot{};BufferPool small(tiny,tiny_slot,1);
    FrameView marker;marker.metadata.available=false;
    PortFrameView delivery{"long-id",{0,0},PortDirection::Output,NodeKind::NetworkInput,{},false,marker,false,{}};
    assert(retain_port_frame(delivery,payloads,small,first)==Status::buffer_too_small);
    assert(!payloads.in_use() && !small.in_use() && !first.identity_storage);
}
static void reduction_and_history_test() {
    GraphDefinition g;g.nodes={node(0,NodeKind::NetworkInput),node(1,NodeKind::ScalarReduce),node(2,NodeKind::ScalarView)};
    g.nodes[0].parameters=NetworkParameters{FrameType::PCM,"","pcm"};
    g.nodes[1].parameters=ReductionParameters{FrameType::PCM,Reduction::Mean};g.edges={{0,1},{1,2}};
    std::array<float,2> left{-2,-4},right{4,10};
    FrameView f;f.type=FrameType::PCM;f.left=left;f.right=right;f.metadata.sample_rate=48000;f.metadata.sequence=1;
    std::array<IngressSpec,1> specs{{{0,contract(f),2}}};
    std::shared_ptr<const PreparedGraph> p;std::unique_ptr<RuntimeContext> c;
    assert(PreparedGraph::prepare(g,specs,{},p,c)==Status::ok);Observation o;
    measuring=true;assert(p->process(0,f,*c,observe,&o)==Status::ok);measuring=false;
    assert(!allocations && o.value[2]==2);
    std::get<ReductionParameters>(g.nodes[1].parameters).operation=Reduction::RMS;
    assert(PreparedGraph::prepare(g,specs,{},p,c)==Status::ok);
    assert(p->process(0,f,*c,observe,&o)==Status::ok);
    assert(std::abs(o.value[2]-std::sqrt(34.f))<1e-5f);
    std::get<ReductionParameters>(g.nodes[1].parameters).operation=Reduction::Mean;
    right={-4,-10};assert(PreparedGraph::prepare(g,specs,{},p,c)==Status::ok);
    assert(p->process(0,f,*c,observe,&o)==Status::ok && o.value[2]==-5);
    left.fill(0);right.fill(0);assert(PreparedGraph::prepare(g,specs,{},p,c)==Status::ok);
    o={};assert(p->process(0,f,*c,observe,&o)==Status::ok && o.calls[2]==1 && o.value[2]==0);

    // History routing overlays only the latest logical row; old rows and
    // producer storage remain untouched, including a wrapped external ring.
    g.nodes={node(0,NodeKind::NetworkInput),node(1,NodeKind::Splitter),node(2,NodeKind::Switch),
        node(3,NodeKind::Gate),node(4,NodeKind::ScalarReduce),node(5,NodeKind::ScalarView)};
    g.nodes[0].parameters=NetworkParameters{FrameType::History,"","history"};
    for(auto i:{1,2,3})g.nodes[i].parameters=RoutingParameters{FrameType::History,2,0};
    g.nodes[4].parameters=ReductionParameters{FrameType::History,Reduction::Mean};
    g.edges={{0,1},{1,2,1,0},{2,3,1,0},{3,4},{4,5}};
    std::array<float,6> rows{100,200,3,5,7,9};std::array<float,2> newest{11,13};
    f={};f.type=FrameType::History;f.history={rows,2,3,3,1};f.history.newest_override=newest;
    f.metadata.sample_rate=48000;f.metadata.sequence=1;f.metadata.bands=2;f.metadata.history_capacity=3;
    specs[0]={0,contract(f),2};
    assert(PreparedGraph::prepare(g,specs,{},p,c)==Status::ok);
    o={};measuring=true;assert(p->process(0,f,*c,observe,&o)==Status::ok);measuring=false;
    assert(!allocations && o.value[5]==0 && rows[0]==100 && newest[0]==11);
    std::get<RoutingParameters>(g.nodes[2].parameters).selected=1;
    assert(PreparedGraph::prepare(g,specs,{},p,c)==Status::ok);
    o={};assert(p->process(0,f,*c,observe,&o)==Status::ok);assert(o.value[5]==12);
    g.nodes[3].enabled=false;
    assert(PreparedGraph::prepare(g,specs,{},p,c)==Status::ok);
    o={};assert(p->process(0,f,*c,observe,&o)==Status::ok);assert(o.value[5]==0);
}
static void unknown_and_legacy_test() {
    GraphDefinition g;g.nodes={node(0,NodeKind::NetworkInput),node(1,NodeKind::Splitter),node(2,NodeKind::MelView)};
    g.nodes[0].parameters=NetworkParameters{FrameType::Mel,"","mel"};g.edges={{0,1},{1,2}};
    std::shared_ptr<const PreparedGraph> p;std::unique_ptr<RuntimeContext> c;
    assert(PreparedGraph::prepare(g,{}, {},p,c)==Status::ok);
    FrameView unavailable;unavailable.type=FrameType::Mel;unavailable.metadata.available=false;
    std::size_t outputs=0,inputs=0;
    auto unknown_output=[](void* u,const PortFrameView& out) {
        ++*static_cast<std::size_t*>(u);
        assert(!out.contract_known && out.contract.type==FrameType::Mel);
        assert(out.direction==PortDirection::Output && validate(out.frame)==Status::unavailable);
    };
    assert(p->process_ports(0,unavailable,*c,{}, {unknown_output,nullptr,&outputs})==Status::unavailable && outputs==3);
    auto unknown_input=[](void* u,const PortFrameView& in) {
        ++*static_cast<std::size_t*>(u);assert(in.direction==PortDirection::Input && !in.contract_known);
    };
    assert(p->process_ports(0,unavailable,*c,{}, {nullptr,unknown_input,&inputs})==Status::unavailable && inputs==1);
    g.nodes={node(0,NodeKind::NetworkInput),node(1,NodeKind::WLED)};
    g.nodes[0].parameters=NetworkParameters{FrameType::Mel,"","mel"};g.nodes[1].parameters=GainParameters{2};g.edges={{0,1}};
    std::array<float,2> values{.25f,.5f};FrameView f;f.type=FrameType::Mel;f.values=values;
    f.metadata.sample_rate=48000;f.metadata.bands=2;f.metadata.sequence=1;
    std::array<IngressSpec,1> specs{{{0,contract(f),2}}};
    assert(PreparedGraph::prepare(g,specs,{},p,c)==Status::ok);Observation legacy;
    assert(p->process(0,f,*c,observe,&legacy)==Status::ok && legacy.value[1]==.5f);
    assert(PreparedGraph::prepare(g,specs,{},p,c)==Status::ok);
    float accepted=0;
    auto sink=[](void* u,const PortFrameView& in){assert(in.direction==PortDirection::Input);*static_cast<float*>(u)=in.frame.values[0];};
    assert(p->process_ports(0,f,*c,{}, {nullptr,sink,&accepted})==Status::ok && accepted==.25f);
    assert(values[0]==.25f && !p->port_contract(1,PortDirection::Output,0));
    assert(p->history(1,0,*c).frames==0);
}
static void packet_boundary_test() {
    GraphDefinition g;g.nodes={node(0,NodeKind::NetworkInput),node(1,NodeKind::Switch),node(2,NodeKind::WLEDOutput)};
    g.nodes[0].parameters=NetworkParameters{FrameType::WledPacket,"","packet"};
    g.nodes[1].parameters=RoutingParameters{FrameType::WledPacket,2,0};g.edges={{0,1},{1,2,1,0}};
    wled_protocol::WledPacket bytes{};
    assert(wled_protocol::silence(87,bytes,{0,44}).status==wled_protocol::Status::Ok);
    bytes[18]=99;
    FrameView f;f.type=FrameType::WledPacket;f.packet=bytes;f.metadata.sample_rate=48000;f.metadata.sequence=1;
    std::array<IngressSpec,1> specs{{{0,contract(f),44}}};
    std::shared_ptr<const PreparedGraph> p;std::unique_ptr<RuntimeContext> c;
    assert(PreparedGraph::prepare(g,specs,{},p,c)==Status::ok);
    Observation o;measuring=true;assert(p->process(0,f,*c,observe,&o)==Status::ok);measuring=false;
    assert(!allocations && o.packet[0]=='0' && o.packet[17]==0 && o.packet[18]==0 && bytes[18]==99);
    assert(wled_protocol::validate_packet(o.packet,{0,44})==wled_protocol::Status::Ok);
    bytes.fill(0);f.metadata.sequence=2;
    assert(validate(f)==Status::ok && p->process(0,f,*c)==Status::invalid_argument);
    // Strict numerical overflow invalidates the packet output and sink, rather
    // than publishing the previously encoded packet or nonfinite signal bytes.
    g.nodes={node(0,NodeKind::NetworkInput),node(1,NodeKind::WLEDEncode),node(2,NodeKind::WLEDOutput)};
    g.nodes[0].parameters=NetworkParameters{FrameType::Mel,"","mel"};
    g.nodes[1].parameters=GainParameters{100};g.edges={{0,1},{1,2}};
    std::array<float,2> mel{std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
    f={};f.type=FrameType::Mel;f.values=mel;f.metadata.sample_rate=48000;f.metadata.bands=2;f.metadata.sequence=1;
    specs[0]={0,contract(f),2};assert(PreparedGraph::prepare(g,specs,{},p,c)==Status::ok);
    std::size_t invalidated=0;
    auto unavailable_packet=[](void* u,const PortFrameView& out) {
        if(out.identity.node==1 || out.identity.node==2) {
            assert(out.frame.type==FrameType::WledPacket && !out.frame.metadata.available && out.frame.packet.empty());
            ++*static_cast<std::size_t*>(u);
        }
    };
    measuring=true;
    assert(p->process_ports(0,f,*c,{}, {unavailable_packet,unavailable_packet,&invalidated})==Status::overflow);
    measuring=false;assert(!allocations && invalidated==2);
}
static void route_check(bool good,const char*) {assert(good);}
static void history_closed(bool use_switch){
 GraphDefinition g;g.graph_id="independent-history";
 g.nodes={node(0,NodeKind::NetworkInput),node(1,NodeKind::Spectrogram),node(2,use_switch?NodeKind::Switch:NodeKind::Gate),node(3,NodeKind::SpectrogramView)};
 g.nodes[0].parameters=NetworkParameters{FrameType::Mel,"","producer"};
 g.nodes[1].parameters=HistoryParameters{4};g.nodes[2].parameters=RoutingParameters{FrameType::History,2,0};
 g.edges={{0,1},{1,2},{2,3}};
 std::array<float,1> value{1};FrameView f;f.type=FrameType::Mel;f.values=value;
 f.metadata.sample_rate=48000;f.metadata.bands=1;f.metadata.sequence=1;f.metadata.producer_epoch=31;f.metadata.epoch=7;f.metadata.graph_revision=19;
 std::array<IngressSpec,1> spec{{{0,contract(f),1}}};
 std::shared_ptr<const PreparedGraph> p,q;std::unique_ptr<RuntimeContext> c,d;
 assert(PreparedGraph::prepare(g,spec,desktop_capabilities(),{},p,c)==Status::ok);
 assert(p->process_ports(0,f,*c,{70,90})==Status::ok);
 f.metadata.sequence=2;value[0]=2;assert(p->process_ports(0,f,*c,{70,90})==Status::ok);
 if(use_switch)std::get<RoutingParameters>(g.nodes[2].parameters).selected=1;else g.nodes[2].enabled=false;
 assert(PreparedGraph::prepare(g,spec,desktop_capabilities(),{},q,d,p.get(),c.get())==Status::ok);
 assert(q->history(2,0,*d).row(1)[0]==2); // A parameter edit does not relabel an already delivered row.
 std::array<float,8> seen{};std::size_t frames=0;
 struct Capture{std::array<float,8>* rows;std::size_t* count;};Capture cap{&seen,&frames};
 auto capture=[](void* u,const PortFrameView& v){auto& x=*static_cast<Capture*>(u);assert(v.direction==PortDirection::Input && v.identity.node==3);assert(v.delivery.epoch==90 && v.frame.metadata.producer_epoch==31 && v.frame.metadata.epoch==7 && v.frame.metadata.graph_revision==19);*x.count=v.frame.history.frames;for(std::size_t i=0;i<*x.count;++i)(*x.rows)[i]=v.frame.history.row(i)[0];};
 measuring=true;f.metadata.sequence=3;value[0]=3;assert(q->process_ports(0,f,*d,{70,90},{nullptr,capture,&cap})==Status::ok);
 f.metadata.sequence=4;value[0]=4;assert(q->process_ports(0,f,*d,{70,90},{nullptr,capture,&cap})==Status::ok);measuring=false;
 std::printf("%s history expected=[1,2,0,0], actual=[%g,%g,%g,%g], frames=%zu\n",use_switch?"Switch":"Gate",seen[0],seen[1],seen[2],seen[3],frames);
 route_check(frames==4 && seen[0]==1 && seen[1]==2 && seen[2]==0 && seen[3]==0,use_switch?"Switch retains prior closed zero row":"Gate retains prior closed zero row");
 const auto before=frames;route_check(q->process_ports(0,f,*d,{70,90},{nullptr,capture,&cap})==Status::would_block && frames==before,"duplicate does not append a row");
 auto snapshot=q->history(2,0,*d);
 assert(snapshot.frames==4 && snapshot.row(2)[0]==0 && snapshot.row(3)[0]==0);
 if(use_switch) {
     const auto other=q->history(2,1,*d);
     assert(other.row(0)[0]==0 && other.row(1)[0]==0 && other.row(2)[0]==3 && other.row(3)[0]==4);
 }
 if(use_switch)std::get<RoutingParameters>(g.nodes[2].parameters).selected=0;else g.nodes[2].enabled=true;
 assert(PreparedGraph::prepare(g,spec,desktop_capabilities(),{},p,c,q.get(),d.get())==Status::ok);
 f.metadata.sequence=5;value[0]=5;measuring=true;
 assert(p->process_ports(0,f,*c,{70,90},{nullptr,capture,&cap})==Status::ok);measuring=false;
 assert(frames==4 && seen[0]==2 && seen[1]==0 && seen[2]==0 && seen[3]==5);
 assert(p->history(1,0,*c).row(1)[0]==3); // producer ring remains intact
 if(use_switch)std::get<RoutingParameters>(g.nodes[2].parameters).selected=1;else g.nodes[2].enabled=false;
 assert(PreparedGraph::prepare(g,spec,desktop_capabilities(),{},q,d,p.get(),c.get())==Status::ok);
 measuring=true;
 for(int tick=6;tick<=7;++tick) {f.metadata.sequence=tick;value[0]=float(tick);assert(q->process_ports(0,f,*d,{70,90},{nullptr,capture,&cap})==Status::ok);}
 measuring=false;assert(seen[0]==0 && seen[1]==5 && seen[2]==0 && seen[3]==0);
 f.metadata.producer_epoch=32;f.metadata.sequence=1;value[0]=8;
 assert(q->process_ports(0,f,*d,{70,90})==Status::ok);
 snapshot=q->history(2,0,*d);assert(snapshot.frames==1 && snapshot.row(0)[0]==0);
 // New masks are admitted before process; failed budget admission is atomic.
 const auto working=q;const auto* working_context=d.get();application::RuntimeLimits tiny;
 tiny.max_memory_bytes=1;
 std::shared_ptr<const PreparedGraph> denied;std::unique_ptr<RuntimeContext> denied_context;
 assert(PreparedGraph::prepare(g,spec,desktop_capabilities(),tiny,denied,denied_context,q.get(),d.get())==Status::exhausted);
 assert(!denied && !denied_context);
 assert(q==working && d.get()==working_context && q->history(2,0,*d).frames==1);
}
static void shrink(bool use_switch){
 GraphDefinition g;g.graph_id="independent-shrink";
 g.nodes={node(0,NodeKind::NetworkInput),node(1,use_switch?NodeKind::Switch:NodeKind::Splitter),node(2,NodeKind::Spectrogram),node(3,NodeKind::Spectrogram)};
 g.nodes[0].parameters=NetworkParameters{FrameType::Mel,"","mel"};g.nodes[1].parameters=RoutingParameters{FrameType::Mel,3,0};
 g.nodes[2].parameters=g.nodes[3].parameters=HistoryParameters{8};g.edges={{0,1},{1,2,0,0},{1,3,1,0}};
 std::array<float,1> values{5};FrameView f;f.type=FrameType::Mel;f.values=values;f.metadata.sample_rate=48000;f.metadata.bands=1;f.metadata.sequence=1;
 std::array<IngressSpec,1> specs{{{0,contract(f),1}}};std::shared_ptr<const PreparedGraph> p,q;std::unique_ptr<RuntimeContext> c,d;
 assert(PreparedGraph::prepare(g,specs,desktop_capabilities(),{},p,c)==Status::ok);assert(p->process_ports(0,f,*c,{})==Status::ok);
 f.metadata.sequence=2;assert(p->process_ports(0,f,*c,{})==Status::ok);
 std::get<RoutingParameters>(g.nodes[1].parameters).outputs=2;
 assert(PreparedGraph::prepare(g,specs,desktop_capabilities(),{},q,d,p.get(),c.get())==Status::ok);
 std::printf("shrink prepare history frames expected=2/2 actual=%zu/%zu resets:",q->history(2,0,*d).frames,q->history(3,0,*d).frames);for(auto id:q->reset_nodes())std::printf(" %d",id);std::puts("");
 route_check(q->history(2,0,*d).frames==2 && q->history(3,0,*d).frames==2,"shrinking unused port preserves compatible surviving branches");
 assert(q->reset_nodes().empty());
 f.metadata.sequence=3;measuring=true;assert(q->process_ports(0,f,*d,{})==Status::ok);measuring=false;
 route_check(q->history(2,0,*d).frames==3 && q->history(3,0,*d).frames==3,"surviving histories append without reset after shrink");
 std::get<RoutingParameters>(g.nodes[1].parameters).outputs=3;
 assert(PreparedGraph::prepare(g,specs,desktop_capabilities(),{},p,c,q.get(),d.get())==Status::ok);
 assert(p->reset_nodes().empty() && p->history(2,0,*c).frames==3 && p->history(3,0,*c).frames==3);
 // Remove connected port2 while preserving sibling port0. The detached known
 // contract keeps its history and receives a new typed-zero row on its clock.
 g.edges.back().output_port=2;
 assert(PreparedGraph::prepare(g,specs,desktop_capabilities(),{},q,d,p.get(),c.get())==Status::ok);
 f.metadata.sequence=4;assert(q->process_ports(0,f,*d,{})==Status::ok);
 std::get<RoutingParameters>(g.nodes[1].parameters).outputs=2;g.edges.pop_back();
 assert(PreparedGraph::prepare(g,specs,desktop_capabilities(),{},p,c,q.get(),d.get())==Status::ok);
 assert(p->reset_nodes().empty() && p->history(2,0,*c).frames==4 && p->history(3,0,*c).frames==4);
 f.metadata.sequence=5;measuring=true;assert(p->process_ports(0,f,*c,{})==Status::ok);measuring=false;
 assert(p->history(2,0,*c).frames==5 && p->history(3,0,*c).frames==5 && p->history(3,0,*c).row(4)[0]==0);
}
static void external_history_test() {
    GraphDefinition g;g.nodes={node(0,NodeKind::NetworkInput),node(1,NodeKind::Switch),
        node(2,NodeKind::SpectrogramView),node(3,NodeKind::SpectrogramView)};
    g.nodes[0].parameters=NetworkParameters{FrameType::History,"","external"};
    g.nodes[1].parameters=RoutingParameters{FrameType::History,2,0};g.edges={{0,1},{1,2},{1,3,1,0}};
    std::array<float,4> rows{1,2,3,4};FrameView f;f.type=FrameType::History;
    f.metadata.sample_rate=48000;f.metadata.bands=1;f.metadata.history_capacity=4;f.metadata.sequence=1;
    f.metadata.epoch=7;f.metadata.producer_epoch=31;f.history={std::span(rows).first(1),1,1,1,0};
    std::array<IngressSpec,1> spec{{{0,contract(f),1}}};
    std::shared_ptr<const PreparedGraph> p,q;std::unique_ptr<RuntimeContext> c,d;
    assert(PreparedGraph::prepare(g,spec,desktop_capabilities(),{},p,c)==Status::ok);
    for(std::size_t tick=1;tick<=2;++tick) {
        f.metadata.sequence=tick;f.history={std::span(rows).first(tick),1,tick,tick,0};
        assert(p->process_ports(0,f,*c,{5,9})==Status::ok);
    }
    std::get<RoutingParameters>(g.nodes[1].parameters).selected=1;
    assert(PreparedGraph::prepare(g,spec,desktop_capabilities(),{},q,d,p.get(),c.get())==Status::ok);
    for(std::size_t tick=3;tick<=4;++tick) {
        f.metadata.sequence=tick;f.history={std::span(rows).first(tick),1,tick,tick,0};
        measuring=true;assert(q->process_ports(0,f,*d,{5,9})==Status::ok);measuring=false;
    }
    assert(q->history(1,0,*d).frames==0); // No runtime ownership of external payload.
    auto routed=q->apply_history_route(1,PortDirection::Output,0,f,*d);
    assert(routed.frames==4 && routed.row(0)[0]==1 && routed.row(1)[0]==2 && routed.row(2)[0]==0 && routed.row(3)[0]==0);
    const auto sink=q->apply_history_route(2,PortDirection::Input,0,f,*d);
    assert(sink.row(2)[0]==0 && sink.row(3)[0]==0);
    const auto other=q->apply_history_route(1,PortDirection::Output,1,f,*d);
    assert(other.row(0)[0]==0 && other.row(1)[0]==0 && other.row(2)[0]==3 && other.row(3)[0]==4);
    FrameView logical=f;logical.history=routed;
    alignas(float) std::array<std::byte,128> storage{};std::array<BufferSlot,1> slots{};BufferPool pool(storage,slots,128);RetainedFrame retained;
    assert(retain(logical,pool,retained)==Status::ok && retained.view.history.row(2)[0]==0);
    assert(retained.view.history.zero_rows.empty());
    for(auto member:{&FrameMetadata::sequence,&FrameMetadata::epoch,&FrameMetadata::producer_epoch,&FrameMetadata::generation}) {
        auto stale=f;++(stale.metadata.*member);
        assert(q->apply_history_route(2,PortDirection::Input,0,stale,*d).frames==0);
    }
    assert(q->apply_history_route(2,PortDirection::Output,0,f,*d).frames==0);
    std::get<RoutingParameters>(g.nodes[1].parameters).selected=0;
    assert(PreparedGraph::prepare(g,spec,desktop_capabilities(),{},p,c,q.get(),d.get())==Status::ok);
    rows={2,3,4,5};f.metadata.sequence=5;
    assert(p->process_ports(0,f,*c,{5,9})==Status::ok);
    routed=p->apply_history_route(1,PortDirection::Output,0,f,*c);
    assert(routed.row(0)[0]==2 && routed.row(1)[0]==0 && routed.row(2)[0]==0 && routed.row(3)[0]==5);
    assert(retained.view.history.row(0)[0]==1 && retained.view.history.row(2)[0]==0);
    // Exact byte accounting of additional per-output masks and failed admission.
    const auto base=p->memory_requirements();
    std::get<RoutingParameters>(g.nodes[1].parameters).outputs=8;
    assert(PreparedGraph::prepare(g,spec,desktop_capabilities(),{},q,d,p.get(),c.get())==Status::ok);
    const auto expanded=q->memory_requirements();assert(expanded.buffer_bytes==base.buffer_bytes+6*4);
    application::RuntimeLimits bounded;bounded.max_memory_bytes=expanded.state_bytes+expanded.buffer_bytes+expanded.scratch_bytes-1;
    std::shared_ptr<const PreparedGraph> denied;std::unique_ptr<RuntimeContext> denied_context;
    assert(PreparedGraph::prepare(g,spec,desktop_capabilities(),bounded,denied,denied_context,p.get(),c.get())==Status::exhausted);
    assert(!denied && !denied_context && p->apply_history_route(1,PortDirection::Output,0,f,*c).row(1)[0]==0);
}

int main() {
    history_closed(false);history_closed(true);shrink(false);shrink(true);
    external_history_test();
    retention_test();
    reduction_and_history_test();
    unknown_and_legacy_test();
    packet_boundary_test();
    GraphDefinition g;
    g.graph_id="routing-graph";
    g.nodes={node(0,NodeKind::NetworkInput),node(1,NodeKind::Splitter),node(2,NodeKind::Switch),
        node(3,NodeKind::Spectrogram),node(4,NodeKind::Spectrogram),node(5,NodeKind::Gate),
        node(6,NodeKind::ScalarReduce),node(7,NodeKind::ScalarView),node(8,NodeKind::ScalarView),
        node(9,NodeKind::MelView),node(10,NodeKind::WLEDEncode),node(11,NodeKind::WLEDOutput)};
    g.nodes[0].parameters=NetworkParameters{FrameType::Mel,"","mel"};
    g.nodes[3].parameters=g.nodes[4].parameters=HistoryParameters{4};
    g.edges={{0,1},{1,2,1,0},{2,3,0,0},{2,4,1,0},{1,5},{5,6},{6,7},{3,8,1,0},{1,9},{1,10},{10,11}};
    std::array<float,3> values{.2f,.4f,.6f};
    FrameView f;f.type=FrameType::Mel;f.values=values;f.metadata.bands=3;f.metadata.sample_rate=48000;f.metadata.sequence=1;
    const std::array<IngressSpec,1> specs{{{0,contract(f),3}}};
    std::shared_ptr<const PreparedGraph> plan,next;
    std::unique_ptr<RuntimeContext> state,updated;
    assert(PreparedGraph::prepare(g,specs,{},plan,state)==Status::ok);
    f.metadata.graph_revision=101;f.metadata.epoch=103;
    PortObservation ports;
    measuring=true;
    assert(plan->process_ports(0,f,*state,{17,29},{observe_output,observe_input,&ports})==Status::ok);
    measuring=false;
    assert(!allocations && ports.output[3][0]==1 && ports.output[3][1]==1);
    assert(ports.history_types[0]==FrameType::History && ports.history_types[1]==FrameType::Scalar);
    assert(!ports.output[7][0] && !ports.output[11][0] && ports.input[7][0]==1 && ports.input[11][0]==1);
    assert(plan->port_contract(3,PortDirection::Output,1)->type==FrameType::Scalar);
    assert(plan->history(3,1,*state).frames==0);
    // Legacy characterization starts from a separately prepared context.
    assert(PreparedGraph::prepare(g,specs,{},plan,state)==Status::ok);
    Observation o;
    measuring=true;assert(plan->process(0,f,*state,observe,&o)==Status::ok);measuring=false;
    assert(!allocations && o.calls[9]==1 && o.value[9]==.2f);
    assert(o.value[3]==.2f && o.value[4]==0 && o.value[7]==.6f && o.value[8]==1);
    assert(o.packet[0]=='0' && o.packet[4]=='2' && o.packet[17]==0);
    // Changing a live switch and gate preserves both accumulator rings.
    std::get<RoutingParameters>(g.nodes[2].parameters).selected=1;
    g.nodes[5].enabled=false;
    assert(PreparedGraph::prepare(g,specs,{},next,updated,plan.get(),state.get())==Status::ok);
    assert(next->history(3,*updated).frames==1 && next->history(4,*updated).frames==1);
    f.metadata.sequence=2;o={};
    measuring=true;assert(next->process(0,f,*updated,observe,&o)==Status::ok);measuring=false;
    assert(!allocations && o.rows[3]==2 && o.rows[4]==2 && o.value[3]==0 && o.value[4]==.2f && o.value[7]==0 && o.value[8]==2);
    assert(next->history(3,0,*updated).row(0)[0]==.2f && next->history(3,0,*updated).row(1)[0]==0);
    assert(next->history(4,0,*updated).row(0)[0]==0 && next->history(4,0,*updated).row(1)[0]==.2f);
    assert(next->history(3,1,*updated).frames==0);
    // A view is a sink, including FFTView. No numerical DSP is performed by it.
    auto invalid=g;invalid.edges.push_back({7,9});
    assert(validate_graph(invalid,desktop_capabilities()).error==GraphError::MissingNodeOrPort);
    invalid=g;invalid.edges.back()={6,11};
    assert(validate_graph(invalid,desktop_capabilities()).error==GraphError::IncompatiblePorts);
    auto caps=desktop_capabilities();caps.node_kinds.erase(std::find(caps.node_kinds.begin(),caps.node_kinds.end(),NodeKind::WLEDEncode));
    assert(validate_graph(g,caps).error==GraphError::UnsupportedKind);
    invalid=g;invalid.processing_version=2;assert(validate_graph(invalid,desktop_capabilities()).error==GraphError::UnsupportedVersion);
    // Caller-owned codec buffers: scalar and packet use explicitly negotiated v3.
    std::array<std::uint8_t,256> bytes{};std::array<float,16> samples{};std::array<char,10> id{};
    FrameView scalar;scalar.type=FrameType::Scalar;scalar.values=std::span(values).first(1);scalar.metadata.sample_rate=48000;scalar.metadata.sequence=1;
    auto encoded=bdsp_protocol::encode(scalar,bytes);assert(encoded && bytes[0]==3);
    FrameView decoded;assert(bdsp_protocol::decode(std::span(bytes).first(encoded.written_bytes),samples,id,decoded));
    assert(decoded.type==FrameType::Scalar && decoded.values[0]==.2f);
    scalar.type=FrameType::WledPacket;scalar.values={};scalar.packet=o.packet;
    encoded=bdsp_protocol::encode(scalar,bytes);assert(encoded && bytes[0]==3);
    assert(bdsp_protocol::decode(std::span(bytes).first(encoded.written_bytes),samples,id,decoded));
    assert(std::equal(decoded.packet.begin(),decoded.packet.end(),o.packet.begin()));
    bytes[0]=2;assert(bdsp_protocol::decode(std::span(bytes).first(encoded.written_bytes),samples,id,decoded).status==Status::unsupported_version);
    // Retention owns packet bytes; borrowed storage is never queued as a span.
    alignas(float) std::array<std::byte,128> storage{};std::array<BufferSlot,1> slots{};
    BufferPool pool(storage,slots,128);RetainedFrame retained;
    assert(retain(scalar,pool,retained)==Status::ok);
    const auto original=o.packet[18];o.packet[18]=99;assert(retained.view.packet[18]==original);
    f.metadata.available=false;f.metadata.sequence=0;f.values={};o={};
    assert(next->process(0,f,*updated,observe,&o)==Status::unavailable && o.calls[7]==1 && o.calls[11]==1);
    ports={};ports.unavailable=true;
    assert(next->process_ports(0,f,*updated,{17,29},{observe_output,observe_input,&ports})==Status::unavailable);
    assert(ports.output[3][0]==1 && ports.output[3][1]==1 && ports.input[11][0]==1 && !ports.output[11][0]);
    assert(ports.history_types[0]==FrameType::History && ports.history_types[1]==FrameType::Scalar);
    std::cout<<"typed routes, live switching, scalar/history ports and WLED packets passed\n";
}
