#include <algorithm>
#include "numeric_transport_fixture.h"
#include "numeric_preview_cadence_fixture.h"
#include <control/numeric_desktop_host.h>
#include <core_app/numeric_default_graph.h>
#include <domain/graph/node_factory.h>
#include <bit>
#include <set>
#include <cstdio>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <thread>
using namespace transport_fixture;
// Regression: a scalar history (NumberAccumulator -> VectorLineView) must be
// queryable as a 1-D owning snapshot, not only matrix Spectrogram histories.
static domain::NumericProcessingGraph matrix_query_graph();
static void feed_query_matrix(NumericDesktopHost&,const domain::NumericProcessingGraph&);
static void selective_snapshot_query() {
 NumericDesktopHost host;auto g=matrix_query_graph();auto other=g.nodes[1];other.id=2;
 g.nodes.push_back(std::move(other));g.edges.push_back({0,2,0,0});
 {Replies replies(host);replies.accepted(apply(g));}
 const auto first=sink(g);const EndpointIdentity second{g.graph_id,2,domain::PortDirection::Input,0};
 require(host.subscribe_numeric_frames({first,second}),"selective snapshot subscription");
 std::array<unsigned,4> delivered{};
 host.set_numeric_frame_handler([&](auto frame){if(frame.context && frame.frame && frame.status==Status::ok)++delivered[frame.context->endpoint.node];});
 feed_query_matrix(host,g);
 require(wait([&]{return delivered[1] && delivered[2];}),"selective fixture current values");settle(30);delivered={};
 for(unsigned i=0;i<100;++i)host.request_numeric_snapshot(first);
 require(wait([&]{return delivered[1]!=0;}),"targeted cold return without new ingress");settle(30);
 require(delivered[1]==1 && delivered[2]==0,"targeted refresh repeated or queried an unrelated endpoint");
 host.request_numeric_snapshots();
 require(wait([&]{return delivered[1]>1 && delivered[2]!=0;}),"full cold recovery lost selected endpoints");
}
static void number_accumulator_vector_history_snapshot() {
 auto g=graph("number-accumulator-preview");
 const domain::ProcessingAdmissionLimits admission{32,64,16,16,256,65536,
     bdsp_protocol::numeric_control::desktop_limits().frame};
 domain::NodeFactorySettings settings;settings.history_capacity=4;
 domain::ConfiguredProcessingNode history;
 require(bool(domain::make_processing_node("NumberAccumulator",2,admission,history,settings)),
         "NumberAccumulator existing factory");
 history.parameters=domain::HistoryProcessingParameters{4};
 require(bool(domain::configure_processing_input(history,g.nodes[0].outputs[0].contract.view(),admission)),
         "NumberAccumulator scalar input contracts");
 g.nodes[1].inputs[0].contract=history.outputs[0].contract;
 g.nodes.push_back(std::move(history));
 g.edges={{0,2,0,0},{2,1,0,0}};
 NumericDesktopHost host;{Replies replies(host);replies.accepted(apply(g));}
 require(host.subscribe_numeric_frames({sink(g)}),"subscribe scalar History vector sink");
 for(std::uint64_t i=1;i<=6;++i)feed(*host.local_engine(),1,i,i);
 DeliveredNumericFrame snapshot;std::uint64_t ordinal=0,filled=0;
 require(host.query_numeric(sink(g),snapshot,&ordinal,&filled)==Status::ok,
         "Scalar History must be registered for snapshot query");
 require(filled==4 && ordinal==6,"NumberAccumulator preserves capacity and exact ordinal");
 const auto& view=std::get<NumericView>(snapshot.frame.view().payload);
 require(view.contract->semantic==Semantic::History && view.contract->shape.rank==Rank::Vector &&
         view.actual[0]==4,"NumberAccumulator emits vector history");
 for(std::size_t index=0;index<4;++index){
     std::uint64_t actual=0;
     require(load_element(view,{0,index,0},actual)==Status::ok && actual==index+3,
             "One incoming number shifts existing history values");
 }
 bool delivered=false;
 host.set_numeric_history_snapshot_handler([&](auto frame,std::uint64_t received_ordinal,
                                                 std::uint64_t received_filled){
     if(frame.context && frame.context->endpoint==sink(g) &&
        received_ordinal==6 && received_filled==4 && frame.status==Status::ok)
         delivered=true;
 });
 host.request_numeric_snapshots();
 require(wait([&]{return delivered;}),"GUI must receive refreshed vector History snapshot");
 selective_snapshot_query();
}
static void event_preview_pending_and_reentrancy(){
 NumericEngine engine;auto g=graph("event.preview");{Replies replies(engine);replies.accepted(apply(g));}
 require(engine.subscribe_numeric_frames({sink(g)}),"event preview subscription");
 // A one-second deadline makes this a queued-wake test, rather than an
 // assertion that a particular platform can sustain an 8ms timer.
 engine.set_numeric_preview_cadence_hz(1);
 std::size_t calls=0;std::uint64_t latest=0;
 engine.set_numeric_frame_handler([&](const auto& f){++calls;latest=value(f);});
 feed(engine,1,1,1);QCoreApplication::processEvents();
 require(calls==1&&latest==1,"idle preview waited for a periodic timer instead of queued wake");
 for(std::uint64_t i=2;i<=40;++i)feed(engine,1,i,i);
 QCoreApplication::processEvents();
 require(calls==1,"pending preview burst ignored its existing deadline");
 // Updating cadence must re-evaluate already pending work; no new ingress is
 // supplied after this call, so a lost wake cannot be hidden by another frame.
 engine.set_numeric_preview_cadence_hz(240);
 require(wait([&]{return latest==40;}),"pending latest preview lost after cadence change");
 require(calls==2,"latest-only burst published intermediate scalar revisions");

 settle(10);std::size_t depth=0,maxDepth=0,reentrantCalls=0;
 engine.set_numeric_frame_handler([&](const auto& f){
  ++depth;maxDepth=std::max(maxDepth,depth);++reentrantCalls;latest=value(f);
  if(latest==41){
   feed(engine,1,42,42);
   engine.set_numeric_preview_cadence_hz(240);
   QCoreApplication::processEvents();
   // Retire the copied callback while a nested event loop has run. The
   // generation/handler fence must also cover frames queued by this callback.
   engine.subscribe_numeric_frames({});
  }
  --depth;
 });
 feed(engine,1,41,41);require(wait([&]{return reentrantCalls!=0;}),"reentrant preview initial delivery");
 settle(20);
 require(maxDepth==1&&reentrantCalls==1&&latest==41,"reentrant preview bypassed flush or subscription fence");
}
static void matrix_lineage_transition(NumericDesktopHost& producer,NumericDesktopHost& host,
 const NumericContract& c,const FrameLimits& limits,const std::array<std::uint64_t,6>& values,
 const PlaneLayout& plane,FrameStamp stamp,std::size_t& frames,std::size_t& history) {
  // The same physical Matrix endpoint may carry an explicit ring and later a
  // dense current value. Retirement must reflect the latest accepted lineage.
  auto arena=std::make_shared<remote_numeric::Arena>(limits);OwnedDataFrame ring;
  stamp.sequence=stamp.source_sequence=2;
  require(retain_frame({stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{2,3},{2,0,{}}}},limits,arena->payload_pool,arena->metadata_pool,ring)==Status::ok,"Matrix ring transition retention");
  require(producer.feed_numeric_owned(0,ring,2,arena,2)==Status::ok,"Matrix explicit ring transition");
  require(wait([&]{return history>0;}),"Matrix ring transition event missing");
  const auto beforeDense=frames;stamp.sequence=stamp.source_sequence=3;
  require(producer.feed_numeric(0,{stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{2,3}}})==Status::ok,"Matrix dense transition");
  require(wait([&]{return frames>beforeDense;}),"Matrix dense transition missing");settle();
  const auto beforeRetire=history;host.disconnect_remote();settle();
  require(history==beforeRetire,"remote retirement invented History after dense Matrix transition");
}
static domain::NumericProcessingGraph slow_history_graph() {
 auto g=graph("slow.history",2);auto h=contract(2);h.semantic=Semantic::History;h.history_element=Semantic::Numeric;
 h.shape={Rank::Matrix,{Axis::Time,Axis::Declared},{ExtentBounds{0,16},ExtentBounds{2,2}}};
 auto count=contract();count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;
 const auto own=[](const NumericContract& c){domain::OwnedProcessingContract result;
  require(domain::own_processing_contract(c,bdsp_protocol::numeric_control::desktop_limits().frame,16384,result)==Status::ok,"slow History contract");return result;};
 domain::ConfiguredProcessingNode history;history.id=2;history.kind="Spectrogram";history.parameters=domain::HistoryProcessingParameters{16};
 history.inputs=g.nodes[1].inputs;history.outputs={{0,domain::PortDirection::Output,own(h)},{1,domain::PortDirection::Output,own(count)}};
 g.nodes[1].inputs[0].contract=history.outputs[0].contract;g.nodes.push_back(std::move(history));g.edges={{0,2,0,0},{2,1,0,0}};
 return g;
}
static void produce_history(NumericEngine& engine,std::uint64_t epoch,std::uint64_t& sequence,
 std::atomic_bool& stop,std::atomic_bool& bad_feed) {auto c=contract(2);PlaneLayout plane{0,{8,0}};FrameStamp stamp;stamp.producer_epoch=epoch;stamp.generation=1;
  auto deadline=std::chrono::steady_clock::now();
  while(!stop.load()){++sequence;std::array<std::uint64_t,2> values{sequence,sequence+1};stamp.sequence=stamp.source_sequence=sequence;
   if(engine.feed_numeric(0,{stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{2,1}}})!=Status::ok)bad_feed=true;
   deadline+=std::chrono::milliseconds(2);while(!stop.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  }
}
static domain::NumericProcessingGraph resume_graph(const domain::ProcessingAdmissionLimits& admission) {
auto defaults=numeric_default_graph();auto history=*std::find_if(defaults.nodes.begin(),defaults.nodes.end(),[](const auto& n){return n.id==7;}),output=*std::find_if(defaults.nodes.begin(),defaults.nodes.end(),[](const auto& n){return n.id==8;});
 history.parameters=domain::HistoryProcessingParameters{64};
 require(bool(domain::derive_processing_node_contracts(history,admission)),"resume History contract derivation");
 output.inputs[0].contract=history.outputs[0].contract;
 domain::ConfiguredProcessingNode input;input.id=1;input.kind="NetworkInput";input.outputs.push_back({0,domain::PortDirection::Output,history.inputs[0].contract});domain::NumericProcessingGraph g;g.graph_id="main";g.nodes={input,history,output};g.edges={{1,7,0,0},{7,8,0,0}};
 return g;
}
template<class Process>
static void resume_provider_boundaries(NumericDesktopHost& host,DeliveredNumericFrame& full,
 std::uint64_t& fullOrdinal,std::uint64_t& fullFilled,std::uint64_t& row,FrameStamp& compactStamp,
 std::uint64_t& generation,std::uint64_t& producerEpoch,bool& discontinuity,std::uint64_t& lastSequence,const Process& process) {
 // Provider incarnations change source sequence, not compatible History's
 // accepted-row ordinal. Keep an independent reference of all accepted values.
 std::vector<float> expected;for(int value=1;value<=81;++value)expected.push_back(float(value));
 std::uint64_t expectedOrdinal=81;
 for(int boundary=0;boundary<3;++boundary){
  row=0;if(boundary==1)++generation;if(boundary==2)++producerEpoch;
  discontinuity=boundary==0;const auto sourceSequence=boundary?1:lastSequence+1;
  process(sourceSequence);expected.push_back(float(sourceSequence));++expectedOrdinal;discontinuity=false;
  require(wait([&]{return row==expectedOrdinal;}),"compatible boundary compact row lost after resync baseline");
  require(compactStamp.discontinuity&&compactStamp.producer_epoch==producerEpoch&&compactStamp.generation==generation,"compact boundary provenance/discontinuity lost");
  process(sourceSequence+1);expected.push_back(float(sourceSequence+1));++expectedOrdinal;
  require(wait([&]{return row==expectedOrdinal;}),"post-boundary compact ordinal continuity lost");
  require(!compactStamp.discontinuity,"boundary flag leaked into next compatible row");
  full={};fullOrdinal=0;host.request_numeric_snapshots();
  require(wait([&]{return full.frame&&fullOrdinal==expectedOrdinal;}),"compatible boundary authoritative snapshot missing");
  require(fullFilled==64,"provider boundary reset compatible History occupancy");
  const auto snapshot=std::get<NumericView>(full.frame.view().payload);require(snapshot.actual[0]==64,"provider boundary changed capacity");
  for(std::size_t index=0;index<64;++index){float actual=0;require(load_element(snapshot,{0,index,0},actual)==Status::ok&&actual==expected[expected.size()-64+index],"provider boundary mutated retained History values/order");}
  const auto stamp=full.frame.view().stamp;require(stamp.producer_epoch==producerEpoch&&stamp.generation==generation,"cold boundary snapshot provenance changed");
 }
}
static void verify_ordinary_matrix_query(NumericDesktopHost& host,std::size_t& frames,const std::size_t& history,const EndpointIdentity& endpoint,const DeliveredNumericFrame& latest,const std::array<std::uint64_t,6>& values) {
 const auto before=frames;
 host.request_numeric_snapshots();require(wait([&]{return frames>before;}),"ordinary Matrix cold query response");
 require(history==0,"passive tag or Matrix rank triggered History path");
 const auto numeric=std::get<NumericView>(latest.frame.view().payload);require(!numeric.ring.capacity && numeric.actual[0]==2 && numeric.actual[1]==3,"ordinary Matrix geometry changed");
 for(size_t row=0;row<2;++row)for(size_t col=0;col<3;++col){std::uint64_t exact=0;require(load_element(numeric,{0,row,col},exact)==Status::ok && exact==values[row*3+col],"ordinary Matrix query lost exact bits");}

}
static void observe_ordinary_matrix(NumericDesktopHost& host,DeliveredNumericFrame& latest,std::size_t& frames,std::size_t& history,std::size_t& deliveries,Status& lastStatus) {
 host.set_numeric_frame_handler([&](const auto& value){++deliveries;lastStatus=value.status;if(value.frame && value.status==Status::ok){latest=value;++frames;}});
 host.set_numeric_history_handler([&](const auto&){++history;});
 host.set_numeric_history_snapshot_handler([&](const auto&,auto,auto){++history;});

}
static void ordinary_matrix_query(bool remote,Semantic tag){
 std::fprintf(stderr,"ordinary Matrix case remote=%d tag=%d\n",int(remote),int(tag));
 auto c=contract(6);c.semantic=tag;c.history_element=Semantic::Numeric;
 c.shape={Rank::Matrix,{Axis::Declared,Axis::Declared},{ExtentBounds{2,2},ExtentBounds{3,3}}};
 const auto limits=bdsp_protocol::numeric_control::desktop_limits().frame;
 domain::OwnedProcessingContract owner;require(domain::own_processing_contract(c,limits,16384,owner)==Status::ok,"ordinary Matrix contract");
 auto g=graph("matrix.query");g.nodes[0].outputs[0].contract=owner;g.nodes[1].inputs[0].contract=owner;
 NumericDesktopHost producer;{Replies replies(producer);replies.accepted(apply(g));}
 std::unique_ptr<NumericDesktopHost> receiver;
 if(remote){require(producer.listen(QHostAddress::LocalHost,0),"Matrix query server");receiver=std::make_unique<NumericDesktopHost>(AudioSourceFactory{},"127.0.0.1",producer.server_port());receiver->connect_remote();require(wait([&]{return receiver->snapshot().graph_revision==producer.snapshot().graph_revision;}),"Matrix query connected");}
 auto& host=remote?*receiver:producer;const auto endpoint=sink(g);size_t frames=0,history=0,deliveries=0;Status lastStatus=Status::unavailable;DeliveredNumericFrame latest;
 observe_ordinary_matrix(host,latest,frames,history,deliveries,lastStatus);
 require(host.subscribe_numeric_frames({endpoint}),"ordinary Matrix subscription");
 // A successful subscribe call only queues the remote binding. Await the real
 // ACK/query completion boundary before sending the single loss-sensitive frame.
 if(remote){Replies ready(host);ready.accepted(command(EngineCommand::Snapshot));}
 settle();
 std::array<std::uint64_t,6> values{9007199254740993ULL,2,3,4,5,UINT64_MAX};PlaneLayout plane{0,{24,8}};
 FrameStamp stamp;stamp.producer_epoch=731;stamp.generation=1;stamp.sequence=stamp.source_sequence=1;
 require(producer.feed_numeric(0,{stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{2,3}}})==Status::ok,"ordinary Matrix feed");
 if(!wait([&]{return frames>0;})){
  DeliveredNumericFrame cached;const auto queried=producer.query_numeric(endpoint,cached);const auto local=producer.snapshot(),visible=host.snapshot();
  std::fprintf(stderr,"Matrix live failed remote=%d tag=%d callbacks=%zu history=%zu status=%d query=%d cached=%d producerState=%d visibleState=%d epoch=%llu/%llu revision=%llu/%llu warning=%s\n",int(remote),int(tag),deliveries,history,int(lastStatus),int(queried),int(bool(cached.frame)),int(local.statuses.value("analyzer").state),int(visible.statuses.value("analyzer").state),(unsigned long long)local.epoch,(unsigned long long)visible.epoch,(unsigned long long)local.graph_revision,(unsigned long long)visible.graph_revision,qPrintable(host.network_warning()));
  throw std::runtime_error("ordinary Matrix live delivery");
 }
 verify_ordinary_matrix_query(host,frames,history,endpoint,latest,values);

 if(remote)matrix_lineage_transition(producer,host,c,limits,values,plane,stamp,frames,history);

}
static void owning_history_ingress(){
 auto c=contract(2);c.semantic=Semantic::History;c.history_element=Semantic::Numeric;
 c.shape.rank=Rank::Matrix;c.shape.axes={Axis::Time,Axis::Declared};c.shape.extents={ExtentBounds{0,96},ExtentBounds{2,2}};
 const auto limits=bdsp_protocol::numeric_control::desktop_limits().frame;
 domain::OwnedProcessingContract owner;require(domain::own_processing_contract(c,limits,16384,owner)==Status::ok,"host History ingress contract");
 auto g=graph("host.history");g.nodes[0].outputs[0].contract=owner;g.nodes[1].inputs[0].contract=owner;
 NumericDesktopHost host;{Replies replies(host);replies.accepted(apply(g));}
 auto arena=std::make_shared<remote_numeric::Arena>(limits);std::array<std::uint64_t,192> values{};
 for(std::size_t row=32;row<96;++row){values[2*row]=9007199254740993ULL+row;values[2*row+1]=UINT64_MAX-row;}
 PlaneLayout plane{0,{16,8}};FrameStamp stamp;stamp.producer_epoch=731;stamp.generation=2;stamp.sequence=stamp.source_sequence=1;
 OwnedDataFrame owned;require(retain_frame({stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{96,2},{96,0,{}}}},limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok,"host owning History retention");
 require(host.feed_numeric_owned(0,owned,81,arena,64)==Status::ok,"host full capacity partial occupancy ingress");
 // The engine must retain ownership after the caller's original payload changes.
 owned={};values.fill(0);DeliveredNumericFrame full;std::uint64_t ordinal=0,filled=0;
 require(host.query_numeric(sink(g),full,&ordinal,&filled)==Status::ok && ordinal==81 && filled==64,"host lost explicit occupancy or ordinal");
 const auto view=std::get<NumericView>(full.frame.view().payload);require(view.actual[0]==96 && view.actual[1]==2,"host padded History geometry");
 for(std::size_t row=0;row<96;++row){std::uint64_t a=0,b=0;
  require(load_element(view,{0,row,0},a)==Status::ok && load_element(view,{0,row,1},b)==Status::ok &&
          a==(row<32?0:9007199254740993ULL+row) && b==(row<32?0:UINT64_MAX-row),"host owning History lost exact padded suffix");
 }
}
static void capacity(){NumericDesktopHost host;std::set<quint64> submitted,completed;bool bad=false;
 host.set_reply_handler([&](const EngineReply& r){bad|=!r.request_id||!submitted.contains(r.request_id)||!completed.insert(r.request_id).second;});
 for(int i=0;i<128;++i){const auto id=host.submit(command(EngineCommand::Snapshot));require(id!=0,"reserved completion admission");submitted.insert(id);}
 require(host.submit(command(EngineCommand::Snapshot))==0,"overflow command dispatched without reply reservation");
 require(wait([&]{return completed.size()==128;})&&!bad&&completed==submitted,"admitted completion lost correlation");
 const auto id=host.submit(command(EngineCommand::Snapshot));require(id&&submitted.insert(id).second,"admission did not recover after drain");require(wait([&]{return completed.size()==129;}),"recovered completion");
}
static void live_replies(){NumericDesktopHost host;auto first=graph("first"),last=graph("last");std::size_t count=0;bool bad=false;
 host.set_reply_handler([&](const EngineReply& r){++count;bad|=!r.accepted||r.snapshot.graph["graph_id"]!="last"||r.snapshot.statuses.value("analyzer").state!=State::Running;});
 require(host.submit(apply(first))&&host.submit(apply(last))&&host.submit(command(EngineCommand::Snapshot)),"live completion submissions");
 QThread::msleep(30);require(wait([&]{return count==3;})&&!bad,"old completion rolled back newer graph or Stop");
}
static void reentrant(const char* mode){NumericDesktopHost host;Replies replies(host);auto g=graph();replies.accepted(apply(g));require(host.subscribe_numeric_frames({sink(g)}),"host subscription");
 std::size_t frames=0;bool changed=false;quint64 target=0;
 host.set_numeric_frame_handler([&](auto){++frames;});
 host.set_reply_handler([&](const EngineReply& r){if(r.request_id!=target||changed)return;changed=true;
  if(QByteArray(mode)=="stop")host.subscribe_numeric_frames({});
  else if(QByteArray(mode)=="apply"){g.nodes[1].enabled=false;host.submit(apply(g));}
  else host.subscribe_numeric_frames({});});
 feed(*host.local_engine(),1,1,41);feed(*host.local_engine(),1,2,42);QThread::msleep(40);
 target=host.submit(command(EngineCommand::Snapshot));QThread::msleep(20);
 require(wait([&]{return changed;}),"reentrant command completion");settle();require(frames==0,"popped data suffix delivered after reentrant Stop/Apply/subscription");
}
static void lifetime(){auto host=std::make_unique<NumericDesktopHost>();auto g=graph();{Replies replies(*host);replies.accepted(apply(g));}require(host->subscribe_numeric_frames({sink(g)}),"lifetime subscription");
 // Remove Replies before endpoint destruction; callback owns no borrowed payload.
 host->set_reply_handler({});std::size_t calls=0;
 host->set_numeric_frame_handler([&](auto){++calls;host.reset();});
 feed(*host->local_engine(),1,1,1);feed(*host->local_engine(),1,2,2);QThread::msleep(40);
 require(wait([&]{return !host;}),"reentrant host destruction");require(calls==1,"callback suffix after host destruction");
 // Replies references an endpoint, so its lifetime cannot outlive that endpoint.
}
static void newest_and_rebind(){NumericDesktopHost host;auto g=graph();{Replies replies(host);replies.accepted(apply(g));}require(host.subscribe_numeric_frames({sink(g)}),"newest subscription");
 std::size_t calls=0;bool unavailable=false;
 host.set_numeric_frame_handler([&](const DeliveredNumericFrame& f){++calls;unavailable=f.status==Status::unavailable||(f.frame&&!f.frame.view().stamp.available);});
 for(std::uint64_t i=1;i<=32;++i)feed(*host.local_engine(),1,i,i);QThread::msleep(40);
 auto c=contract();FrameStamp stamp;stamp.sequence=33;stamp.source_sequence=33;stamp.producer_epoch=host.snapshot().epoch;stamp.generation=1;stamp.available=false;
 require(host.feed_numeric(0,{stamp,NumericView{&c,{},{},{1,1}}})==Status::ok,"unavailable terminal ingress");QThread::msleep(40);
 require(wait([&]{return calls>0;})&&unavailable&&calls<=8,"bounded GUI saturation lost newest terminal unavailable");
 calls=0;host.set_numeric_frame_handler([&](auto){++calls;host.set_numeric_frame_handler({});});
 feed(*host.local_engine(),1,34,34);feed(*host.local_engine(),1,35,35);QThread::msleep(40);require(wait([&]{return calls==1;}),"rebind first frame");settle();require(calls==1,"old handler continued after reentrant unset");
 std::size_t replacement=0;host.set_numeric_frame_handler([&](auto){++replacement;});feed(*host.local_engine(),1,36,36);require(wait([&]{return replacement==1;}),"replacement handler did not receive fresh frame");
}
static domain::NumericProcessingGraph matrix_query_graph(){
 auto c=contract(6);c.shape={Rank::Matrix,{Axis::Declared,Axis::Declared},{ExtentBounds{2,2},ExtentBounds{3,3}}};
 domain::OwnedProcessingContract owner;require(domain::own_processing_contract(c,bdsp_protocol::numeric_control::desktop_limits().frame,16384,owner)==Status::ok,"async Matrix contract");
 auto g=graph("async.matrix",6);g.nodes[0].outputs[0].contract=owner;g.nodes[1].inputs[0].contract=owner;return g;
}
static void feed_query_matrix(NumericDesktopHost& host,const domain::NumericProcessingGraph& g){
 const auto c=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
 std::array<std::uint64_t,6> values{41,42,43,44,45,46};PlaneLayout plane{0,{24,8}};FrameStamp stamp;
 stamp.producer_epoch=host.snapshot().epoch;stamp.generation=1;stamp.sequence=stamp.source_sequence=1;
 require(host.feed_numeric(0,{stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{2,3}}})==Status::ok,"async Matrix feed");
}
static void asynchronous_snapshot_query(){
 NumericDesktopHost host;auto g=matrix_query_graph();{Replies replies(host);replies.accepted(apply(g));}
 require(host.subscribe_numeric_frames({sink(g)}),"async snapshot subscription");
 feed_query_matrix(host,g);settle();
 {DeliveredNumericFrame cached;require(host.query_numeric(sink(g),cached)==Status::ok&&value(cached)==41,"async fixture must support owning cold Matrix query");}
 struct Gate {std::mutex mutex;std::condition_variable changed;bool entered=false,released=false;};
 auto gate=std::make_shared<Gate>();
 require(QMetaObject::invokeMethod(host.local_engine(),[gate]{std::unique_lock lock(gate->mutex);gate->entered=true;gate->changed.notify_all();gate->changed.wait_for(lock,std::chrono::seconds(2),[&]{return gate->released;});},Qt::QueuedConnection),"worker gate queued");
 {std::unique_lock lock(gate->mutex);require(gate->changed.wait_for(lock,std::chrono::seconds(1),[&]{return gate->entered;}),"worker gate entered");}
 std::size_t callbacks=0;host.set_numeric_frame_handler([&](const auto& f){if(f.frame&&value(f)==41)++callbacks;});
 for(int i=0;i<1000;++i)host.request_numeric_snapshots();
 QCoreApplication::processEvents();const bool bypassed_worker=callbacks!=0;
 {std::lock_guard lock(gate->mutex);gate->released=true;}gate->changed.notify_all();
 require(!bypassed_worker,"GUI queried local processing snapshot instead of worker");
 require(wait([&]{return callbacks>0;}),"coalesced snapshot response missing");settle();
 require(callbacks<=2,"request storm accumulated snapshot jobs");
 // Retire a requested owning snapshot before GUI delivery. Same worker result
 // cannot resurrect a removed subscription even if epoch/revision are unchanged.
 callbacks=0;host.request_numeric_snapshots();QCoreApplication::processEvents();
 require(host.subscribe_numeric_frames({}),"async snapshot unsubscribe");callbacks=0;settle();
 require(callbacks==0,"retired async snapshot delivered after unsubscribe");
}
static void slow_history_preview_preserves_taps(){
 auto g=slow_history_graph();
 NumericEngine engine;{Replies replies(engine);replies.accepted(apply(g));}
 require(engine.subscribe_numeric_frames({sink(g)}),"slow History GUI subscription");engine.set_numeric_preview_cadence_hz(1);
 std::uint64_t sequence=0,tap_rows=0;bool bad=false,preview_overflow=false;
 engine.set_numeric_history_handler([&](const auto& row){preview_overflow|=row.compact.status==Status::exhausted;});
 const auto tap=engine.subscribe_numeric_tap({sink(g)},{},[&](const auto& row){
  bad|=row.compact.status!=Status::ok;
  if(row.event==NumericHistoryEvent::Row){++tap_rows;bad|=!row.compact.frame||row.accepted_row_ordinal!=tap_rows;}
 });require(tap!=0,"slow History tap subscription");
 std::atomic_bool stop{false},bad_feed{false};const auto epoch=engine.snapshot().epoch;
 std::thread producer([&]{produce_history(engine,epoch,sequence,stop,bad_feed);});
 settle(600);stop=true;producer.join();settle(40);
 require(sequence>160,"slow History fixture did not exceed GUI arena capacity");
 require(!bad_feed&&!bad&&tap_rows==sequence,"slow GUI History owners starved loss-sensitive tap arena");
 engine.set_numeric_preview_cadence_hz(120);require(wait([&]{return preview_overflow;}),"slow GUI overflow lost resync marker");
 engine.unsubscribe_numeric_tap(tap);
}
static std::pair<domain::NumericProcessingGraph,std::vector<EndpointIdentity>> fanout_graph(int count=24){auto g=graph();g.nodes.resize(1);g.edges.clear();std::vector<EndpointIdentity> selected;
 for(int i=0;i<count;++i){auto output=graph().nodes[1];output.id=100+i;g.nodes.push_back(output);g.edges.push_back({0,output.id,0,0});selected.push_back({g.graph_id,output.id,domain::PortDirection::Input,0});}
 return {std::move(g),std::move(selected)};
}
static void wide_host_preview_fairness(){
 auto [g,selected]=fanout_graph(65);NumericDesktopHost host;{Replies replies(host);replies.accepted(apply(g));}
 require(host.subscribe_numeric_frames(selected),"wide host subscription");host.set_numeric_preview_cadence_hz(240);
 FrameProbe peer(*host.local_engine(),{g.graph_id,0,domain::PortDirection::Output,0});
 struct Seen {unsigned count=0;std::uint64_t first=0,last=0;};std::array<Seen,65> seen{};bool invalid=false;
 host.set_numeric_frame_handler([&](const DeliveredNumericFrame& frame){
  if(!frame.context || frame.context->endpoint.node<100 || frame.context->endpoint.node>=165)return;
  if(frame.status!=Status::ok || !frame.frame || !frame.frame.view().stamp.available){invalid=true;return;}
  auto& lane=seen[std::size_t(frame.context->endpoint.node-100)];std::uint64_t exact=0;
  if(load_element(std::get<NumericView>(frame.frame.view().payload),{0,0,0},exact)!=Status::ok || exact!=frame.frame.view().stamp.source_sequence || (lane.count && exact<=lane.last)){invalid=true;return;}
  if(!lane.count)lane.first=exact;lane.last=exact;++lane.count;
 });
 auto complete=[&]{return std::all_of(seen.begin(),seen.end(),[](const auto& lane){return lane.count>=5 && lane.last>lane.first;});};
 QElapsedTimer deadline;deadline.start();std::uint64_t sequence=0;
 while(!complete() && !invalid && deadline.elapsed()<5000){
  ++sequence;feed(host,1,sequence,sequence);
  // GUI intentionally paused. The independent loss-sensitive peer must keep
  // advancing while the worker fills the same fixed 32 GUI owning slots.
  QElapsedTimer peerDeadline;peerDeadline.start();while(!peer.equals(sequence) && peerDeadline.elapsed()<500)QThread::msleep(1);
  require(peer.equals(sequence),"paused wide GUI owners starved independent peer");
  QCoreApplication::processEvents();
 }
 if(invalid || !complete()){std::fprintf(stderr,"wide host rounds=%llu invalid=%d",(unsigned long long)sequence,int(invalid));for(std::size_t i=0;i<seen.size();++i)std::fprintf(stderr," %zu:%u/%llu..%llu",i,seen[i].count,(unsigned long long)seen[i].first,(unsigned long long)seen[i].last);std::fprintf(stderr,"\n");}
 require(!invalid && complete(),"65 producing endpoints did not each deliver five progressing GUI frames");
 require(peer.count()>=sequence,"wide GUI preview lost peer frames");host.set_numeric_frame_handler({});
 std::fprintf(stderr,"65-endpoint host fairness: five progressing frames each, rounds=%llu, paused GUI peer preserved\n",(unsigned long long)sequence);
}
static void local_delivery_saturation(){NumericEngine engine;auto [g,selected]=fanout_graph();{Replies replies(engine);replies.accepted(apply(g));}require(engine.subscribe_numeric_frames(selected),"core saturation selected ports");
 std::map<domain::NodeId,std::uint64_t> latest;std::set<domain::NodeId> gaps;engine.set_numeric_frame_handler([&](const auto& f){if(f.context&&f.frame&&f.frame.view().stamp.available){latest[f.context->endpoint.node]=value(f);if(f.frame.view().stamp.discontinuity)gaps.insert(f.context->endpoint.node);}});
 for(std::uint64_t i=1;i<=200;++i)feed(engine,1,i,i);
 require(wait([&]{return latest.size()==24&&std::all_of(latest.begin(),latest.end(),[](const auto& item){return item.second==200;});}),"core ordinary backlog exhausted delivery arena or lost latest ports");
 require(gaps.empty(),"GUI preview coalescing mutated signal discontinuity");
}
static void evicted_endpoint_gap(){NumericEngine engine;auto [g,selected]=fanout_graph(0);selected.push_back({g.graph_id,0,domain::PortDirection::Output,0});
 for(int i=0;i<43;++i){domain::ConfiguredProcessingNode splitter;splitter.id=100+i;splitter.kind="Splitter";splitter.parameters=domain::RouteProcessingParameters{3,0,true};splitter.inputs=graph().nodes[1].inputs;for(int j=0;j<3;++j){auto output=graph().nodes[0].outputs.front();output.id=j;splitter.outputs.push_back(output);selected.push_back({g.graph_id,splitter.id,domain::PortDirection::Output,domain::PortId(j)});}g.nodes.push_back(splitter);g.edges.push_back({0,splitter.id,0,0});}
 {Replies replies(engine);replies.accepted(apply(g));}require(engine.subscribe_numeric_frames(selected),"eviction selected ports");
 bool delivered=false,gap=false;engine.set_numeric_frame_handler([&](const auto& f){if(f.context&&f.context->endpoint==selected.front()&&f.frame){delivered=value(f)==2;gap=f.frame.view().stamp.discontinuity;}});
 feed(engine,1,1,1);require(engine.subscribe_numeric_frames({selected.front()}),"eviction selected endpoint");feed(engine,1,2,2);
 require(wait([&]{return delivered;}),"evicted endpoint fresh delivery");require(!gap,"GUI preview eviction mutated signal discontinuity");
}
static void resume_ingress(std::uint64_t sequence,std::uint64_t& lastSequence,auto& values,std::uint64_t producerEpoch,std::uint64_t generation,bool discontinuity,NumericDesktopHost& producer,const NumericContract& c,const PlaneLayout& plane,const auto& frequencyBytes,bool remote) {
lastSequence=sequence;values.fill(float(sequence));FrameStamp stamp;stamp.producer_epoch=producerEpoch;stamp.generation=generation;stamp.discontinuity=discontinuity;stamp.sequence=stamp.source_sequence=sequence;stamp.timestamp_ticks=sequence;const auto status=producer.feed_numeric(1,{stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{50,1},{},CoordinateView{{std::span<const std::byte>(frequencyBytes),{}}}}});if(status!=Status::ok)std::fprintf(stderr,"resume ingress remote=%d sequence=%llu producer=%llu generation=%llu discontinuity=%d status=%d\n",int(remote),(unsigned long long)sequence,(unsigned long long)producerEpoch,(unsigned long long)generation,int(discontinuity),int(status));require(status==Status::ok,"resume ingress");}

static void resume_warm_history(const auto& process,std::uint64_t& row,DeliveredNumericFrame& full,std::uint64_t& fullOrdinal,std::uint64_t& fullFilled,bool remote) {
 process(1);require(wait([&]{return row==1||fullOrdinal==1;}),"resume initial delivery");full={};fullOrdinal=row=0;
 for(std::uint64_t i=2;i<=80;++i){process(i);QThread::msleep(3);}QThread::msleep(40);
 require(wait([&]{return bool(full.frame)&&fullOrdinal==80;}),"GUI History overflow did not restore authoritative full snapshot");auto view=std::get<NumericView>(full.frame.view().payload);float first=0,last=0;const auto firstStatus=load_element(view,{0,0,0},first),lastStatus=load_element(view,{0,63,0},last);
 if(fullOrdinal!=80||view.actual[0]!=64||first!=17||last!=80)std::fprintf(stderr,"remote=%d full ordinal=%llu rows=%llu first=%f last=%f\n",int(remote),(unsigned long long)fullOrdinal,(unsigned long long)view.actual[0],first,last);
 require(fullOrdinal==80&&fullFilled==64&&view.actual[0]==64&&firstStatus==Status::ok&&first==17&&lastStatus==Status::ok&&last==80,"resume lost History ring rows");require(full.frame.view().stamp.producer_epoch==701,"resume changed producer");
 process(81);require(wait([&]{return row==81;}),"compact History did not continue after resync");
}

static void resume_capacity_changes(auto& g,const NumericContract& hc,NumericDesktopHost& producer,NumericDesktopHost& host,const EndpointIdentity& selected,DeliveredNumericFrame& full,std::uint64_t& fullOrdinal,std::uint64_t& fullFilled) {
 // A full 64-row buffer expanded at ordinal81 has64 real rows, not81.
 // Exercise the actual local/remote cold bridge, including wire metadata.
 auto resize=[&](std::uint64_t capacity){
  auto resized=hc;resized.shape.extents[0].maximum=capacity;
  domain::OwnedProcessingContract contract;require(domain::own_processing_contract(resized,bdsp_protocol::numeric_control::desktop_limits().frame,16384,contract)==Status::ok,"resize bridge contract");
  g.nodes[1].outputs[0].contract=contract;g.nodes[1].parameters=domain::HistoryProcessingParameters{capacity};g.nodes[2].inputs[0].contract=contract;
  {Replies replies(producer);replies.accepted(apply(g));}
  require(wait([&]{return host.snapshot().graph_revision==producer.snapshot().graph_revision;}),"resize remote revision");
  // Selecting an unchanged set intentionally does not request another query.
  // Rebind explicitly so the oracle observes a new cold snapshot after Apply.
  require(host.subscribe_numeric_frames({}),"resize cold unsubscribe");
  full={};fullOrdinal=fullFilled=0;require(host.subscribe_numeric_frames({selected}),"resize cold subscription");
  require(wait([&]{return full.frame && fullOrdinal==81;}),"resize full snapshot absent");
 };
 resize(96);auto view=std::get<NumericView>(full.frame.view().payload);
 require(view.actual[0]==96 && fullFilled==64,"expanded snapshot fabricated occupancy from capacity or ordinal");
 float padding=1,retainedFirst=0,retainedLast=0;
 require(load_element(view,{0,0,0},padding)==Status::ok && padding==0 &&
         load_element(view,{0,32,0},retainedFirst)==Status::ok && retainedFirst==18 &&
         load_element(view,{0,95,0},retainedLast)==Status::ok && retainedLast==81,"resize snapshot lost fresh suffix or zero padding");
 resize(64);require(fullFilled==64,"shrink snapshot occupancy");
}

static void selected_port_burst(NumericDesktopHost& producer,std::uint64_t& sequence) {
std::fprintf(stderr,"phase selected burst enter\n");for(std::uint64_t i=1;i<=20;++i)feed(*producer.local_engine(),1,++sequence,i);std::fprintf(stderr,"phase selected burst done\n");}
static void selected_port_frame(const auto& f,auto& latest,auto& retained,auto& overflow_gaps) {
if(f.context&&f.frame&&f.frame.view().stamp.available){latest[f.context->endpoint.node]=value(f);retained[f.context->endpoint.node]=f;if(f.frame.view().stamp.discontinuity)overflow_gaps.insert(f.context->endpoint.node);}
}

static void verify_selected_port_recovery(bool remote,NumericDesktopHost& producer,NumericDesktopHost& host,const std::map<domain::NodeId,std::uint64_t>& latest,const std::set<domain::NodeId>& overflow_gaps,std::uint64_t& sequence) {
 const auto allLatest=[&](std::uint64_t expected){return latest.size()==24&&std::all_of(latest.begin(),latest.end(),[&](const auto& entry){return entry.second==expected;});};
 if(remote){std::fprintf(stderr,"phase selected settling\n");settle(1000);require(latest.size()==24,"FIFO overload lost endpoint bindings");for(const auto& [node,last]:latest)require(last<=20,"FIFO overload fabricated future data");}
 else require(wait([&]{return allLatest(20);}),"local preview lost newest selected ports");
 std::fprintf(stderr,"phase selected fresh21 remote=%d\n",int(remote));feed(*producer.local_engine(),1,++sequence,21);require(wait([&]{return allLatest(21);}),"renderer owners exhausted receiver arena");if(remote)require(!overflow_gaps.empty(),"loss-sensitive FIFO overload omitted recovery gap");
 std::fprintf(stderr,"phase selected complete remote=%d\n",int(remote));
}
static void all_selected_ports(bool remote){std::fprintf(stderr,"phase selected begin remote=%d\n",int(remote));NumericDesktopHost producer;auto [g,selected]=fanout_graph();
 {Replies replies(producer);replies.accepted(apply(g));}std::unique_ptr<NumericDesktopHost> receiver;
 if(remote){require(producer.listen(QHostAddress::LocalHost,0),"selected ports actual server");receiver=std::make_unique<NumericDesktopHost>(AudioSourceFactory{},"127.0.0.1",producer.server_port());receiver->connect_remote();require(wait([&]{return receiver->snapshot().graph_revision==producer.snapshot().graph_revision;}),"selected ports remote connected");}
 auto& host=remote?*receiver:producer;require(host.subscribe_numeric_frames(selected),"all selected ports subscription");
 std::map<domain::NodeId,std::uint64_t> latest;std::set<domain::NodeId> overflow_gaps;std::map<domain::NodeId,DeliveredNumericFrame> rendererOwners;host.set_numeric_frame_handler([&](const DeliveredNumericFrame& f){selected_port_frame(f,latest,rendererOwners,overflow_gaps);});
 std::uint64_t sequence=0;
 // CONTROL State is not DATA subscription readiness. Establish every real
 // owning callback before pausing the GUI, without any extra observer tap.
 if(remote&&!wait([&]{feed(*producer.local_engine(),1,++sequence,0);return latest.size()==24;})){
  const auto state=host.snapshot();std::fprintf(stderr,"remote baseline observed=%zu epoch=%llu revision=%llu analyzer=%d warning=%s\n",latest.size(),(unsigned long long)state.epoch,(unsigned long long)state.graph_revision,int(state.statuses.value("analyzer").state),host.network_warning().toUtf8().constData());for(const auto& [node,last]:latest)std::fprintf(stderr,"node=%u value=%llu\n",unsigned(node),(unsigned long long)last);throw std::runtime_error("remote selected ports baseline delivery");}
 std::fprintf(stderr,"phase selected baseline remote=%d\n",int(remote));overflow_gaps.clear();auto burst=[&]{selected_port_burst(producer,sequence);};if(remote)require(QMetaObject::invokeMethod(producer.local_engine(),burst,Qt::BlockingQueuedConnection),"bounded worker burst admission");else burst();QThread::msleep(40);
 verify_selected_port_recovery(remote,producer,host,latest,overflow_gaps,sequence);
}
static void all_selected_cold_matrices(){
 auto g=matrix_query_graph();auto sink_node=g.nodes[1];g.nodes.resize(1);g.edges.clear();std::vector<EndpointIdentity> selected;
 for(int i=0;i<24;++i){sink_node.id=100+i;g.nodes.push_back(sink_node);g.edges.push_back({0,sink_node.id,0,0});selected.push_back({g.graph_id,sink_node.id,domain::PortDirection::Input,0});}
 NumericDesktopHost host;{Replies replies(host);replies.accepted(apply(g));}
 require(host.subscribe_numeric_frames(selected),"cold Matrix fanout subscription");feed_query_matrix(host,g);settle();
 for(const auto& endpoint:selected){DeliveredNumericFrame cached;require(host.query_numeric(endpoint,cached)==Status::ok&&value(cached)==41,"cold Matrix fanout query precondition");}
 std::set<domain::NodeId> restored;host.set_numeric_frame_handler([&](const auto& f){if(f.context&&f.frame&&value(f)==41)restored.insert(f.context->endpoint.node);});
 host.request_numeric_snapshots();require(wait([&]{return restored.size()==24;}),"bounded cold query batches evicted selected Matrix endpoints");
}
static void history_resume(bool remote){NumericDesktopHost producer;
 const auto capabilities=producer.local_engine()->capabilities();require(capabilities.numeric.has_value(),"resume numeric profile");
 auto g=resume_graph(capabilities.numeric->runtime.admission);
 const auto inputOwner=g.nodes[1].inputs[0].contract;const auto historyOwner=g.nodes[1].outputs[0].contract;
 auto c=std::get<NumericContract>(inputOwner.view());auto hc=std::get<NumericContract>(historyOwner.view());
 {Replies replies(producer);replies.accepted(apply(g));}std::unique_ptr<NumericDesktopHost> receiver;
 if(remote){require(producer.listen(QHostAddress::LocalHost,0),"resume actual server");receiver=std::make_unique<NumericDesktopHost>(AudioSourceFactory{},"127.0.0.1",producer.server_port());receiver->connect_remote();require(wait([&]{return receiver->snapshot().graph_revision==producer.snapshot().graph_revision;}),"resume remote connected");}
 auto& host=remote?*receiver:producer;const EndpointIdentity selected{"main",8,domain::PortDirection::Input,0};require(host.subscribe_numeric_frames({selected}),"resume subscription");
 DeliveredNumericFrame full;signal_contracts::FrameStamp compactStamp;std::uint64_t row=0,fullOrdinal=0,fullFilled=0;host.set_numeric_history_snapshot_handler([&](auto f,std::uint64_t ordinal,std::uint64_t filled_count){if(f.context&&f.context->endpoint==selected){full=std::move(f);fullOrdinal=ordinal;fullFilled=filled_count;}});host.set_numeric_history_handler([&](auto r){if(r.compact.context&&r.compact.context->endpoint==selected){row=r.accepted_row_ordinal;compactStamp=r.compact.frame.view().stamp;compactStamp.stream_id={};}});
 std::uint64_t producerEpoch=701,generation=9;bool discontinuity=false;
 std::uint64_t lastSequence=0;
 std::array<std::byte,50*8> frequencyBytes{};
 for(std::size_t band=0;band<50;++band){const auto bits=std::bit_cast<std::uint64_t>(double(band+1)*100.);for(std::size_t byte=0;byte<8;++byte)frequencyBytes[band*8+byte]=std::byte((bits>>(byte*8))&255);}
 std::array<float,50> values{};PlaneLayout plane{0,{4,0}};auto process=[&](std::uint64_t sequence){resume_ingress(sequence,lastSequence,values,producerEpoch,generation,discontinuity,producer,c,plane,frequencyBytes,remote);};
 resume_warm_history(process,row,full,fullOrdinal,fullFilled,remote);
 resume_capacity_changes(g,hc,producer,host,selected,full,fullOrdinal,fullFilled);
 resume_provider_boundaries(host,full,fullOrdinal,fullFilled,row,compactStamp,generation,producerEpoch,discontinuity,lastSequence,process);


}
static void active_remote_rebind(){NumericDesktopHost producer;auto g=graph();g.nodes.clear();g.edges.clear();std::vector<EndpointIdentity> selected;
 for(int i=0;i<60;++i){auto pair=graph();pair.nodes[0].id=i;pair.nodes[1].id=100+i;g.nodes.push_back(pair.nodes[0]);g.nodes.push_back(pair.nodes[1]);g.edges.push_back({domain::NodeId(i),domain::NodeId(100+i),0,0});selected.push_back({g.graph_id,domain::NodeId(100+i),domain::PortDirection::Input,0});}
 {Replies replies(producer);replies.accepted(apply(g));}require(producer.listen(QHostAddress::LocalHost,0),"active rebind server");NumericDesktopHost receiver({},"127.0.0.1",producer.server_port());require(receiver.subscribe_numeric_frames(selected),"active initial60 subscription");receiver.connect_remote();require(wait([&]{return receiver.snapshot().graph_revision==producer.snapshot().graph_revision;}),"active rebind connected");
 std::map<domain::NodeId,DeliveredNumericFrame> retained;receiver.set_numeric_frame_handler([&](const auto& f){if(f.context&&f.frame&&f.frame.view().stamp.available)retained[f.context->endpoint.node]=f;});
 std::array<std::uint64_t,60> source_sequences{};auto deliver=[&](std::uint64_t sequence,std::uint64_t expected){for(int i=0;i<60;++i){require(wait([&]{feed(producer,1,++source_sequences[i],expected,i);auto found=retained.find(100+i);return found!=retained.end()&&value(found->second)==expected;}),"active rebind selected port delivery");}};
 deliver(1,20);require(retained.size()==60,"active rebind60 renderer owners");require(receiver.subscribe_numeric_frames(selected),"same60 active arena admission refused");deliver(2,21);for(const auto& [node,f]:retained)require(value(f)==21,"same60 rebind lost retained renderer or latest delivery");
}
int main(int argc,char**argv){std::setvbuf(stderr,nullptr,_IONBF,0);QCoreApplication app(argc,argv);try{if(argc>1&&QByteArray(argv[1])=="--vector-history"){number_accumulator_vector_history_snapshot();return 0;}number_accumulator_vector_history_snapshot();if(argc>1&&QByteArray(argv[1])=="--wide-preview"){wide_host_preview_fairness();return 0;}wide_host_preview_fairness();event_preview_pending_and_reentrancy();cadence_fixture::preview_cadence();asynchronous_snapshot_query();slow_history_preview_preserves_taps();for(bool remote:{false,true})for(auto tag:{Semantic::Numeric,Semantic::History})ordinary_matrix_query(remote,tag);if(argc>1&&QByteArray(argv[1])=="--eviction-gap"){evicted_endpoint_gap();return 0;}std::fprintf(stderr,"phase eviction\n");evicted_endpoint_gap();std::fprintf(stderr,"phase saturation\n");local_delivery_saturation();std::fprintf(stderr,"phase capacity\n");capacity();std::fprintf(stderr,"phase live\n");live_replies();std::fprintf(stderr,"phase reentrant\n");reentrant("stop");reentrant("apply");reentrant("subscribe");std::fprintf(stderr,"phase lifetime\n");lifetime();std::fprintf(stderr,"phase rebind\n");newest_and_rebind();all_selected_ports(false);all_selected_ports(true);all_selected_cold_matrices();std::fprintf(stderr,"phase active_remote_rebind\n");active_remote_rebind();std::fprintf(stderr,"phase owning History ingress\n");owning_history_ingress();std::fprintf(stderr,"phase history local\n");history_resume(false);std::fprintf(stderr,"phase history remote\n");history_resume(true);std::puts("worker admission/live completion/reentrant data fences/lifetime/newest/rebind/all selected ports/active60 rebind/local and remote History resume PASS");return 0;}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
