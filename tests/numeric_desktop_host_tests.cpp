#include "numeric_transport_fixture.h"
#include <control/numeric_desktop_host.h>
#include <core_app/numeric_default_graph.h>
#include <bit>
#include <set>
#include <cstdio>
using namespace transport_fixture;
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
 host.set_numeric_frame_handler([&](const auto& value){++deliveries;lastStatus=value.status;if(value.frame && value.status==Status::ok){latest=value;++frames;}});
 host.set_numeric_history_handler([&](const auto&){++history;});
 host.set_numeric_history_snapshot_handler([&](const auto&,auto,auto){++history;});
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
 }const auto before=frames;
 host.request_numeric_snapshots();require(wait([&]{return frames>before;}),"ordinary Matrix cold query response");
 require(history==0,"passive tag or Matrix rank triggered History path");
 const auto numeric=std::get<NumericView>(latest.frame.view().payload);require(!numeric.ring.capacity && numeric.actual[0]==2 && numeric.actual[1]==3,"ordinary Matrix geometry changed");
 for(size_t row=0;row<2;++row)for(size_t col=0;col<3;++col){std::uint64_t exact=0;require(load_element(numeric,{0,row,col},exact)==Status::ok && exact==values[row*3+col],"ordinary Matrix query lost exact bits");}

 if(remote){
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
static std::pair<domain::NumericProcessingGraph,std::vector<EndpointIdentity>> fanout_graph(int count=24){auto g=graph();g.nodes.resize(1);g.edges.clear();std::vector<EndpointIdentity> selected;
 for(int i=0;i<count;++i){auto output=graph().nodes[1];output.id=100+i;g.nodes.push_back(output);g.edges.push_back({0,output.id,0,0});selected.push_back({g.graph_id,output.id,domain::PortDirection::Input,0});}
 return {std::move(g),std::move(selected)};
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
static void all_selected_ports(bool remote){std::fprintf(stderr,"phase selected begin remote=%d\n",int(remote));NumericDesktopHost producer;auto [g,selected]=fanout_graph();
 {Replies replies(producer);replies.accepted(apply(g));}std::unique_ptr<NumericDesktopHost> receiver;
 if(remote){require(producer.listen(QHostAddress::LocalHost,0),"selected ports actual server");receiver=std::make_unique<NumericDesktopHost>(AudioSourceFactory{},"127.0.0.1",producer.server_port());receiver->connect_remote();require(wait([&]{return receiver->snapshot().graph_revision==producer.snapshot().graph_revision;}),"selected ports remote connected");}
 auto& host=remote?*receiver:producer;require(host.subscribe_numeric_frames(selected),"all selected ports subscription");
 std::map<domain::NodeId,std::uint64_t> latest;std::set<domain::NodeId> overflow_gaps;std::map<domain::NodeId,DeliveredNumericFrame> rendererOwners;host.set_numeric_frame_handler([&](const DeliveredNumericFrame& f){if(f.context&&f.frame&&f.frame.view().stamp.available){latest[f.context->endpoint.node]=value(f);rendererOwners[f.context->endpoint.node]=f;if(f.frame.view().stamp.discontinuity)overflow_gaps.insert(f.context->endpoint.node);}});
 std::uint64_t sequence=0;
 // CONTROL State is not DATA subscription readiness. Establish every real
 // owning callback before pausing the GUI, without any extra observer tap.
 if(remote&&!wait([&]{feed(*producer.local_engine(),1,++sequence,0);return latest.size()==24;})){
  const auto state=host.snapshot();std::fprintf(stderr,"remote baseline observed=%zu epoch=%llu revision=%llu analyzer=%d warning=%s\n",latest.size(),(unsigned long long)state.epoch,(unsigned long long)state.graph_revision,int(state.statuses.value("analyzer").state),host.network_warning().toUtf8().constData());for(const auto& [node,last]:latest)std::fprintf(stderr,"node=%u value=%llu\n",unsigned(node),(unsigned long long)last);throw std::runtime_error("remote selected ports baseline delivery");}
 std::fprintf(stderr,"phase selected baseline remote=%d\n",int(remote));overflow_gaps.clear();auto burst=[&]{std::fprintf(stderr,"phase selected burst enter\n");for(std::uint64_t i=1;i<=20;++i)feed(*producer.local_engine(),1,++sequence,i);std::fprintf(stderr,"phase selected burst done\n");};if(remote)require(QMetaObject::invokeMethod(producer.local_engine(),burst,Qt::BlockingQueuedConnection),"bounded worker burst admission");else burst();QThread::msleep(40);
 const auto allLatest=[&](std::uint64_t expected){return latest.size()==24&&std::all_of(latest.begin(),latest.end(),[&](const auto& entry){return entry.second==expected;});};
 if(remote){std::fprintf(stderr,"phase selected settling\n");settle(1000);require(latest.size()==24,"FIFO overload lost endpoint bindings");for(const auto& [node,last]:latest)require(last<=20,"FIFO overload fabricated future data");}
 else require(wait([&]{return allLatest(20);}),"local preview lost newest selected ports");
 std::fprintf(stderr,"phase selected fresh21 remote=%d\n",int(remote));feed(*producer.local_engine(),1,++sequence,21);require(wait([&]{return allLatest(21);}),"renderer owners exhausted receiver arena");if(remote)require(!overflow_gaps.empty(),"loss-sensitive FIFO overload omitted recovery gap");std::fprintf(stderr,"phase selected complete remote=%d\n",int(remote));
}
static void history_resume(bool remote){auto defaults=numeric_default_graph();auto history=defaults.nodes[6],output=defaults.nodes[7];
 auto c=std::get<NumericContract>(history.inputs[0].contract.view());auto hc=std::get<NumericContract>(history.outputs[0].contract.view());hc.shape.extents[0].maximum=64;
 domain::OwnedProcessingContract owned;require(domain::own_processing_contract(hc,bdsp_protocol::numeric_control::desktop_limits().frame,16384,owned)==Status::ok,"resume History geometry");history.outputs[0].contract=owned;output.inputs[0].contract=owned;history.parameters=domain::HistoryProcessingParameters{64};
 domain::ConfiguredProcessingNode input;input.id=1;input.kind="NetworkInput";input.outputs.push_back({0,domain::PortDirection::Output,history.inputs[0].contract});domain::NumericProcessingGraph g;g.graph_id="main";g.nodes={input,history,output};g.edges={{1,7,0,0},{7,8,0,0}};
 NumericDesktopHost producer;{Replies replies(producer);replies.accepted(apply(g));}std::unique_ptr<NumericDesktopHost> receiver;
 if(remote){require(producer.listen(QHostAddress::LocalHost,0),"resume actual server");receiver=std::make_unique<NumericDesktopHost>(AudioSourceFactory{},"127.0.0.1",producer.server_port());receiver->connect_remote();require(wait([&]{return receiver->snapshot().graph_revision==producer.snapshot().graph_revision;}),"resume remote connected");}
 auto& host=remote?*receiver:producer;const EndpointIdentity selected{"main",8,domain::PortDirection::Input,0};require(host.subscribe_numeric_frames({selected}),"resume subscription");
 DeliveredNumericFrame full;std::uint64_t row=0,fullOrdinal=0,fullFilled=0;host.set_numeric_history_snapshot_handler([&](auto f,std::uint64_t ordinal,std::uint64_t filled_count){if(f.context&&f.context->endpoint==selected){full=std::move(f);fullOrdinal=ordinal;fullFilled=filled_count;}});host.set_numeric_history_handler([&](auto r){if(r.compact.context&&r.compact.context->endpoint==selected)row=r.accepted_row_ordinal;});
 std::uint64_t producerEpoch=701,generation=9;bool discontinuity=false;
 std::uint64_t lastSequence=0;
 std::array<std::byte,50*8> frequencyBytes{};
 for(std::size_t band=0;band<50;++band){const auto bits=std::bit_cast<std::uint64_t>(double(band+1)*100.);for(std::size_t byte=0;byte<8;++byte)frequencyBytes[band*8+byte]=std::byte((bits>>(byte*8))&255);}
 std::array<float,50> values{};PlaneLayout plane{0,{4,0}};auto process=[&](std::uint64_t sequence){lastSequence=sequence;values.fill(float(sequence));FrameStamp stamp;stamp.producer_epoch=producerEpoch;stamp.generation=generation;stamp.discontinuity=discontinuity;stamp.sequence=stamp.source_sequence=sequence;stamp.timestamp_ticks=sequence;const auto status=producer.feed_numeric(1,{stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{50,1},{},CoordinateView{{std::span<const std::byte>(frequencyBytes),{}}}}});if(status!=Status::ok)std::fprintf(stderr,"resume ingress remote=%d sequence=%llu producer=%llu generation=%llu discontinuity=%d status=%d\n",int(remote),(unsigned long long)sequence,(unsigned long long)producerEpoch,(unsigned long long)generation,int(discontinuity),int(status));require(status==Status::ok,"resume ingress");};
 process(1);require(wait([&]{return row==1||fullOrdinal==1;}),"resume initial delivery");full={};fullOrdinal=row=0;
 for(std::uint64_t i=2;i<=80;++i){process(i);QThread::msleep(3);}QThread::msleep(40);
 require(wait([&]{return bool(full.frame)&&fullOrdinal==80;}),"GUI History overflow did not restore authoritative full snapshot");auto view=std::get<NumericView>(full.frame.view().payload);float first=0,last=0;const auto firstStatus=load_element(view,{0,0,0},first),lastStatus=load_element(view,{0,63,0},last);
 if(fullOrdinal!=80||view.actual[0]!=64||first!=17||last!=80)std::fprintf(stderr,"remote=%d full ordinal=%llu rows=%llu first=%f last=%f\n",int(remote),(unsigned long long)fullOrdinal,(unsigned long long)view.actual[0],first,last);
 require(fullOrdinal==80&&fullFilled==64&&view.actual[0]==64&&firstStatus==Status::ok&&first==17&&lastStatus==Status::ok&&last==80,"resume lost History ring rows");require(full.frame.view().stamp.producer_epoch==701,"resume changed producer");
 process(81);require(wait([&]{return row==81;}),"compact History did not continue after resync");
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
 resize(96);view=std::get<NumericView>(full.frame.view().payload);
 require(view.actual[0]==96 && fullFilled==64,"expanded snapshot fabricated occupancy from capacity or ordinal");
 float padding=1,retainedFirst=0,retainedLast=0;
 require(load_element(view,{0,0,0},padding)==Status::ok && padding==0 &&
         load_element(view,{0,32,0},retainedFirst)==Status::ok && retainedFirst==18 &&
         load_element(view,{0,95,0},retainedLast)==Status::ok && retainedLast==81,"resize snapshot lost fresh suffix or zero padding");
 resize(64);require(fullFilled==64,"shrink snapshot occupancy");
 for(int boundary=0;boundary<3;++boundary){
  if(boundary){full={};fullOrdinal=row=0;const auto additional=boundary==1?79:59;for(int i=0;i<additional;++i){process(lastSequence+1);QThread::msleep(3);}QThread::msleep(40);require(wait([&]{return fullOrdinal==80;}),"next boundary full baseline");}
  row=0;full={};fullOrdinal=0;discontinuity=boundary==0;if(boundary==1)++generation;if(boundary==2)++producerEpoch;process(boundary?1:lastSequence+1);discontinuity=false;
  if(!boundary)require(wait([&]{return row==1;}),"resync baseline suppressed discontinuity History row1");
  else{for(std::uint64_t i=2;i<=20;++i){process(i);QThread::msleep(3);}QThread::msleep(40);require(wait([&]{return fullOrdinal==20;}),"resync baseline suppressed lower new-source full ordinal20");
   require(fullFilled==20 && std::get<NumericView>(full.frame.view().payload).actual[0]==64,"partial snapshot must distinguish occupancy from capacity");
   const auto stamp=full.frame.view().stamp;require(stamp.producer_epoch==producerEpoch&&stamp.generation==generation&&!stamp.discontinuity,"new-source full snapshot identity or discontinuity");
   process(21);require(wait([&]{return row==21;}),"new-source compact21 after full20");}
 }
}
static void active_remote_rebind(){NumericDesktopHost producer;auto g=graph();g.nodes.clear();g.edges.clear();std::vector<EndpointIdentity> selected;
 for(int i=0;i<60;++i){auto pair=graph();pair.nodes[0].id=i;pair.nodes[1].id=100+i;g.nodes.push_back(pair.nodes[0]);g.nodes.push_back(pair.nodes[1]);g.edges.push_back({domain::NodeId(i),domain::NodeId(100+i),0,0});selected.push_back({g.graph_id,domain::NodeId(100+i),domain::PortDirection::Input,0});}
 {Replies replies(producer);replies.accepted(apply(g));}require(producer.listen(QHostAddress::LocalHost,0),"active rebind server");NumericDesktopHost receiver({},"127.0.0.1",producer.server_port());require(receiver.subscribe_numeric_frames(selected),"active initial60 subscription");receiver.connect_remote();require(wait([&]{return receiver.snapshot().graph_revision==producer.snapshot().graph_revision;}),"active rebind connected");
 std::map<domain::NodeId,DeliveredNumericFrame> retained;receiver.set_numeric_frame_handler([&](const auto& f){if(f.context&&f.frame&&f.frame.view().stamp.available)retained[f.context->endpoint.node]=f;});
 std::array<std::uint64_t,60> source_sequences{};auto deliver=[&](std::uint64_t sequence,std::uint64_t expected){for(int i=0;i<60;++i){require(wait([&]{feed(producer,1,++source_sequences[i],expected,i);auto found=retained.find(100+i);return found!=retained.end()&&value(found->second)==expected;}),"active rebind selected port delivery");}};
 deliver(1,20);require(retained.size()==60,"active rebind60 renderer owners");require(receiver.subscribe_numeric_frames(selected),"same60 active arena admission refused");deliver(2,21);for(const auto& [node,f]:retained)require(value(f)==21,"same60 rebind lost retained renderer or latest delivery");
}
int main(int argc,char**argv){std::setvbuf(stderr,nullptr,_IONBF,0);std::fprintf(stderr,"phase main before Qt\n");QCoreApplication app(argc,argv);std::fprintf(stderr,"phase Qt ready\n");try{for(bool remote:{false,true})for(auto tag:{Semantic::Numeric,Semantic::History})ordinary_matrix_query(remote,tag);if(argc>1&&QByteArray(argv[1])=="--eviction-gap"){evicted_endpoint_gap();return 0;}std::fprintf(stderr,"phase eviction\n");evicted_endpoint_gap();std::fprintf(stderr,"phase saturation\n");local_delivery_saturation();std::fprintf(stderr,"phase capacity\n");capacity();std::fprintf(stderr,"phase live\n");live_replies();std::fprintf(stderr,"phase reentrant\n");reentrant("stop");reentrant("apply");reentrant("subscribe");std::fprintf(stderr,"phase lifetime\n");lifetime();std::fprintf(stderr,"phase rebind\n");newest_and_rebind();all_selected_ports(false);all_selected_ports(true);std::fprintf(stderr,"phase active_remote_rebind\n");active_remote_rebind();std::fprintf(stderr,"phase owning History ingress\n");owning_history_ingress();std::fprintf(stderr,"phase history local\n");history_resume(false);std::fprintf(stderr,"phase history remote\n");history_resume(true);std::puts("worker admission/live completion/reentrant data fences/lifetime/newest/rebind/all selected ports/active60 rebind/local and remote History resume PASS");return 0;}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
