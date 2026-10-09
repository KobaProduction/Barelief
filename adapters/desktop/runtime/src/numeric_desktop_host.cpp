#include <control/numeric_desktop_host.h>
#include <control/numeric_routing_adapter.h>
#include <control/numeric_remote_endpoint.h>
#include <control/numeric_control_server.h>
#include <pipeline/numeric_engine.h>
#include <aggregator/aggregator_client.h>
#include <QThread>
#include <QPointer>
#include <QUuid>
#include <mutex>
#include <algorithm>
#include <atomic>
namespace pipeline {
struct NumericDesktopHost::Impl {
 NumericDesktopHost& owner;QThread thread;QObject* worker=new QObject;
 std::unique_ptr<EnginePort> endpoint;NumericEngine* local=nullptr;
 NumericRoutingAdapter* routing=nullptr;std::unique_ptr<AggregatorClient> registry;
 std::unique_ptr<NumericControlServer> server;
 mutable std::mutex mutex;EngineSnapshot cache;bool pending=false,retired=false;
 std::size_t outstanding=0;std::array<quint64,128> submitted{};std::uint64_t data_generation=0;
 std::atomic<PortDeliveryMode> delivery_mode{PortDeliveryMode::Unsupported};
 NumericDeliveryMailbox<EngineReply,128> replies;
 // GUI leases cannot exhaust the engine's128-slot arena and starve network
 // peers while drawing is paused. Preserve newest data and terminal markers.
 NumericDeliveryMailbox<DeliveredNumericFrame,32> frames;
 NumericDeliveryMailbox<DeliveredNumericHistoryRow,8> histories;
 struct HistorySnapshot {DeliveredNumericFrame frame;std::uint64_t ordinal,filled_count;bool explicit_history=true;};
 NumericDeliveryMailbox<HistorySnapshot,8> history_snapshots;
 std::vector<EndpointIdentity> selected;
 struct ResumeBaseline {EndpointIdentity endpoint;std::uint64_t ordinal,producer_epoch,generation;};
 std::vector<ResumeBaseline> resumed_ordinals;
 bool history_resync=false,resync_inflight=false;quint64 resync_request=0;
 std::uint64_t resumed_generation=0;
 std::uint64_t snapshot_generation=0,reply_generation=0,frame_generation=0,history_generation=0;
 std::function<void(EngineSnapshot)> snapshot_handler;std::function<void(EngineReply)> reply_handler;
 std::function<void(DeliveredNumericFrame)> frame_handler;std::function<void(DeliveredNumericHistoryRow)> history_handler;
 std::function<void(DeliveredNumericFrame,std::uint64_t,std::uint64_t)> history_snapshot_handler;std::uint64_t history_snapshot_generation=0;
 explicit Impl(NumericDesktopHost& target):owner(target){worker->moveToThread(&thread);QObject::connect(&thread,&QThread::finished,worker,&QObject::deleteLater);thread.start();}
 template<class F> void call(F&& f)const{if(QThread::currentThread()==&thread)f();else QMetaObject::invokeMethod(worker,std::forward<F>(f),Qt::BlockingQueuedConnection);}
 void schedule_locked(){if(pending||retired)return;pending=true;QMetaObject::invokeMethod(&owner,[this]{drain();},Qt::QueuedConnection);}
 void update_cache_locked(EngineSnapshot value){
  if(value.epoch!=cache.epoch||value.graph_revision!=cache.graph_revision||
      (value.statuses.value("analyzer").state==State::Stopped&&cache.statuses.value("analyzer").state!=State::Stopped)){
   ++data_generation;frames.clear();histories.clear();history_snapshots.clear();history_resync=false;
  }cache=std::move(value);
 }
 void publish(EngineSnapshot value){delivery_mode=endpoint->port_delivery_mode();std::lock_guard lock(mutex);update_cache_locked(std::move(value));schedule_locked();}
 void publish(EngineReply value){auto live=endpoint->snapshot();std::lock_guard lock(mutex);update_cache_locked(std::move(live));
  if(value.request_id==resync_request){resync_request=0;resync_inflight=false;schedule_locked();return;}
  const auto id=std::find(submitted.begin(),submitted.end(),value.request_id);if(id==submitted.end())return;*id=0;
  // submit reserves a completion slot before dispatch. Accepted request IDs are
  // never evicted, and internal transport requests do not consume GUI slots.
  replies.push(std::move(value));schedule_locked();}
 void publish(DeliveredNumericFrame value){std::lock_guard lock(mutex);
  // Each selected endpoint retains its newest frame. A fast port cannot evict
  // the other previews merely because the GUI event loop is paused.
  const auto count=frames.size();for(std::size_t i=0;i<count;++i){auto previous=frames.pop();
   if(previous&&(!previous->context||!value.context||previous->context->endpoint!=value.context->endpoint))frames.push(std::move(*previous));}
  if(frames.size()==32)frames.pop();frames.push(std::move(value));schedule_locked();}
 void publish(DeliveredNumericHistoryRow value){std::lock_guard lock(mutex);if(histories.size()==8){histories.pop();history_resync=true;}histories.push(std::move(value));schedule_locked();}
 void publish_history_snapshot(DeliveredNumericFrame value,std::uint64_t ordinal,std::uint64_t filled_count){std::lock_guard lock(mutex);if(history_snapshots.size()==8)history_snapshots.pop();history_snapshots.push({std::move(value),ordinal,filled_count});schedule_locked();}
 void drain(){QPointer<NumericDesktopHost> guard(&owner);auto snapshotCallback=snapshot_handler;auto replyCallback=reply_handler;auto frameCallback=frame_handler;auto historyCallback=history_handler;
  auto historySnapshotCallback=history_snapshot_handler;const auto historySnapshotVersion=history_snapshot_generation;
  const auto snapshotVersion=snapshot_generation,replyVersion=reply_generation,frameVersion=frame_generation,historyVersion=history_generation;
  EngineSnapshot state;{std::lock_guard lock(mutex);pending=false;state=cache;}if(snapshotCallback&&snapshotVersion==snapshot_generation)snapshotCallback(state);if(!guard)return;
  std::uint64_t resumeGeneration;bool resume=false;std::vector<EndpointIdentity> endpoints;
  {std::lock_guard lock(mutex);resumeGeneration=data_generation;if(history_resync&&!resync_inflight){resume=true;history_resync=false;histories.clear();if(local)endpoints=selected;else resync_inflight=true;}}
  if(resumed_generation!=resumeGeneration){resumed_generation=resumeGeneration;resumed_ordinals.clear();}
  auto restore=[&](HistorySnapshot snapshot){
   {std::lock_guard lock(mutex);if(resumeGeneration!=data_generation||snapshot.frame.delivery.epoch!=cache.epoch||snapshot.frame.delivery.graph_revision!=cache.graph_revision||cache.statuses.value("analyzer").state==State::Stopped)return;}
   if(!snapshot.frame.context||!snapshot.frame.frame)return;
   const auto value=snapshot.frame.frame.view();
   const auto* numeric=std::get_if<signal_contracts::NumericView>(&value.payload);
   // A cold query also returns ordinary scalars/vectors/matrices. Only an
   // explicit ring payload enters the history snapshot presentation path.
   if(!snapshot.explicit_history && (!numeric || !numeric->ring.capacity)){
    if(frameCallback&&frameVersion==frame_generation)frameCallback(std::move(snapshot.frame));
    return;
   }
   const auto stamp=value.stamp;
   auto found=std::find_if(resumed_ordinals.begin(),resumed_ordinals.end(),[&](const auto& item){return item.endpoint==snapshot.frame.context->endpoint;});
   if(found!=resumed_ordinals.end()&&found->producer_epoch==stamp.producer_epoch&&found->generation==stamp.generation&&!stamp.discontinuity&&snapshot.ordinal<found->ordinal)return;
   ResumeBaseline baseline{snapshot.frame.context->endpoint,snapshot.ordinal,stamp.producer_epoch,stamp.generation};
   if(found==resumed_ordinals.end())resumed_ordinals.push_back(std::move(baseline));else *found=std::move(baseline);
   if(historySnapshotCallback&&historySnapshotVersion==history_snapshot_generation)historySnapshotCallback(std::move(snapshot.frame),snapshot.ordinal,snapshot.filled_count);
  };
  if(resume&&local){for(const auto& selectedEndpoint:endpoints){DeliveredNumericFrame full;std::uint64_t ordinal=0,filled_count=0;bool explicit_history=false;const auto status=local->query_numeric(selectedEndpoint,full,&ordinal,&filled_count,&explicit_history);if(status==signal_contracts::Status::ok||status==signal_contracts::Status::unavailable)restore({std::move(full),ordinal,filled_count,explicit_history});if(!guard)return;}}
  else if(resume){call([this]{const auto id=endpoint->submit(EngineCommand{});std::lock_guard lock(mutex);resync_request=id;if(!id)resync_inflight=false;});}
  for(std::size_t i=0;i<8;++i){std::optional<HistorySnapshot> full;{std::lock_guard lock(mutex);full=history_snapshots.pop();}if(!full)break;restore(std::move(*full));if(!guard)return;}
  for(std::size_t i=0;i<128;++i){std::optional<EngineReply> reply;std::optional<DeliveredNumericFrame> frame;std::optional<DeliveredNumericHistoryRow> history;std::uint64_t generation;
   {std::lock_guard lock(mutex);reply=replies.pop();frame=frames.pop();history=histories.pop();generation=data_generation;if(reply){--outstanding;reply->snapshot=cache;}}
   if(!reply&&!frame&&!history)break;if(reply&&replyCallback&&replyVersion==reply_generation)replyCallback(std::move(*reply));if(!guard)return;
   auto current=[&](DeliveryStamp delivery){std::lock_guard lock(mutex);return generation==data_generation&&delivery.epoch==cache.epoch&&delivery.graph_revision==cache.graph_revision&&cache.statuses.value("analyzer").state!=State::Stopped;};
   if(frame&&frameCallback&&frameVersion==frame_generation&&current(frame->delivery))frameCallback(std::move(*frame));if(!guard)return;
   if(history&&historyCallback&&historyVersion==history_generation&&current(history->compact.delivery)){
    auto prior=std::find_if(resumed_ordinals.begin(),resumed_ordinals.end(),[&](const auto& item){return history->compact.context&&item.endpoint==history->compact.context->endpoint;});
    bool changed=history->event!=NumericHistoryEvent::Row;
    if(prior!=resumed_ordinals.end()&&history->compact.frame){const auto stamp=history->compact.frame.view().stamp;changed|=stamp.producer_epoch!=prior->producer_epoch||stamp.generation!=prior->generation||stamp.discontinuity;}
    if(changed&&prior!=resumed_ordinals.end()){resumed_ordinals.erase(prior);prior=resumed_ordinals.end();}
    if(history->event!=NumericHistoryEvent::Row||prior==resumed_ordinals.end()||history->accepted_row_ordinal>prior->ordinal)historyCallback(std::move(*history));
   }if(!guard)return;}
 }
 ~Impl(){ {std::lock_guard lock(mutex);retired=true;}call([this]{server.reset();endpoint.reset();registry.reset();routing=nullptr;local=nullptr;});thread.quit();thread.wait();}
};
NumericDesktopHost::NumericDesktopHost(AudioSourceFactory factory,QString host,quint16 port,std::optional<desktop::SerialEndpoint> serial,QObject* parent):QObject(parent),impl_(std::make_unique<Impl>(*this)){
 impl_->call([&,factory=std::move(factory),host=std::move(host),serial=std::move(serial)]()mutable{
  if(!host.isEmpty()) {if(serial)impl_->endpoint=std::make_unique<NumericRemoteEndpoint>(*serial);else impl_->endpoint=std::make_unique<NumericRemoteEndpoint>(host,port);
   static_cast<NumericRemoteEndpoint*>(impl_->endpoint.get())->set_numeric_history_snapshot_handler([this](auto frame,std::uint64_t ordinal,std::uint64_t filled_count){impl_->publish_history_snapshot(std::move(frame),ordinal,filled_count);});}
  else{auto engine=std::make_unique<NumericEngine>(std::move(factory));impl_->local=engine.get();
   impl_->registry=std::make_unique<AggregatorClient>(AggregatorClientConfig{QUuid::createUuid(),ComponentType::data_sink,0});
   auto routing=std::make_unique<NumericRoutingAdapter>(*engine,impl_->registry.get());impl_->routing=routing.get();auto* r=routing.get();
   engine->set_adapter_handlers(std::move(routing),{[r](const QJsonObject& g,QString* e){return r->validate_graph_routes(g,e);},[r](const QJsonObject& g){r->configure_graph_routes(g);},[r](const QJsonObject& c,QString* e){return r->configure(c,e);},[r]{return r->diagnostics().value("error").toString();}});
   impl_->endpoint=std::move(engine);}
  impl_->endpoint->set_snapshot_handler([this](auto v){impl_->publish(std::move(v));});impl_->endpoint->set_reply_handler([this](auto v){impl_->publish(std::move(v));});
  impl_->endpoint->set_numeric_frame_handler([this](auto v){impl_->publish(std::move(v));});impl_->endpoint->set_numeric_history_handler([this](auto v){impl_->publish(std::move(v));});impl_->publish(impl_->endpoint->snapshot());
 });
}
NumericDesktopHost::~NumericDesktopHost()=default;
NumericEngine* NumericDesktopHost::local_engine()const{return impl_->local;}
domain::ProcessingDiagnostic NumericDesktopHost::validate_numeric_node(const domain::ConfiguredProcessingNode& node)const{
 domain::ProcessingDiagnostic result;
 impl_->call([&]{if(impl_->local)result=impl_->local->validate_numeric_node(node);});
 return result;
}

quint64 NumericDesktopHost::submit(EngineCommand command){
 {std::lock_guard lock(impl_->mutex);if(impl_->outstanding==128||impl_->retired)return 0;++impl_->outstanding;}
 quint64 id=0;impl_->call([&]{id=impl_->endpoint->submit(std::move(command));
  {std::lock_guard lock(impl_->mutex);if(id)*std::find(impl_->submitted.begin(),impl_->submitted.end(),quint64{0})=id;else --impl_->outstanding;}
  impl_->publish(impl_->endpoint->snapshot());});return id;
}
void NumericDesktopHost::set_snapshot_handler(std::function<void(EngineSnapshot)> f){++impl_->snapshot_generation;impl_->snapshot_handler=std::move(f);}
void NumericDesktopHost::set_reply_handler(std::function<void(EngineReply)> f){++impl_->reply_generation;impl_->reply_handler=std::move(f);}
void NumericDesktopHost::set_numeric_frame_handler(std::function<void(DeliveredNumericFrame)> f){++impl_->frame_generation;impl_->frame_handler=std::move(f);}
void NumericDesktopHost::set_numeric_history_handler(std::function<void(DeliveredNumericHistoryRow)> f){++impl_->history_generation;impl_->history_handler=std::move(f);}
void NumericDesktopHost::set_numeric_history_snapshot_handler(std::function<void(DeliveredNumericFrame,std::uint64_t,std::uint64_t)> f){++impl_->history_snapshot_generation;impl_->history_snapshot_handler=std::move(f);}
void NumericDesktopHost::request_numeric_snapshots(){std::lock_guard lock(impl_->mutex);impl_->history_resync=true;impl_->schedule_locked();}
EngineSnapshot NumericDesktopHost::snapshot()const{std::lock_guard lock(impl_->mutex);return impl_->cache;}
bool NumericDesktopHost::configure_network(QJsonObject config,QString* error){bool ok=false;QString e;impl_->call([&]{ok=impl_->endpoint->configure_network(std::move(config),&e);});if(error)*error=e;return ok;}
QString NumericDesktopHost::network_warning()const{QString value;impl_->call([&]{value=impl_->endpoint->network_warning();});return value;}
void NumericDesktopHost::connect_remote(){impl_->call([&]{impl_->endpoint->connect_remote();});}
void NumericDesktopHost::disconnect_remote(){impl_->call([&]{impl_->endpoint->disconnect_remote();impl_->publish(impl_->endpoint->snapshot());});}
EnginePort::PortDeliveryMode NumericDesktopHost::port_delivery_mode()const noexcept{return impl_->delivery_mode.load();}
bool NumericDesktopHost::subscribe_numeric_frames(std::vector<EndpointIdentity> endpoints,QString* error){
 std::sort(endpoints.begin(),endpoints.end());endpoints.erase(std::unique(endpoints.begin(),endpoints.end()),endpoints.end());
 bool ok=false;QString e;impl_->call([&]{
  {std::lock_guard lock(impl_->mutex);if(impl_->selected==endpoints){ok=true;return;}}
  ok=impl_->endpoint->subscribe_numeric_frames(endpoints,&e);
  if(ok){std::lock_guard lock(impl_->mutex);impl_->selected=std::move(endpoints);++impl_->data_generation;impl_->frames.clear();impl_->histories.clear();impl_->history_snapshots.clear();impl_->history_resync=!impl_->selected.empty();impl_->schedule_locked();}
 });if(error)*error=e;return ok;
}
void NumericDesktopHost::configure_registry(QHostAddress address,quint16 port,int timeout){impl_->call([&]{if(impl_->registry)impl_->registry->connect_to_aggregator_server(address,port,timeout);});}
void NumericDesktopHost::disconnect_registry(){impl_->call([&]{if(impl_->registry)impl_->registry->disconnect_from_aggregator_server();});}
void NumericDesktopHost::set_registry_capabilities(QJsonObject capabilities){impl_->call([&]{if(impl_->registry)impl_->registry->set_capabilities(std::move(capabilities));});}
void NumericDesktopHost::set_registry_identity(QUuid uuid){impl_->call([&]{if(!impl_->local||uuid.isNull())return;auto old=std::move(impl_->registry);impl_->registry=std::make_unique<AggregatorClient>(AggregatorClientConfig{uuid,ComponentType::core,impl_->server?impl_->server->port():quint16(0)});impl_->routing->set_registry(impl_->registry.get());});}
signal_contracts::Status NumericDesktopHost::feed_numeric(domain::NodeId node,const signal_contracts::DataFrameView& frame){return impl_->local?impl_->local->feed_numeric(node,frame):signal_contracts::Status::unavailable;}
signal_contracts::Status NumericDesktopHost::feed_numeric_owned(domain::NodeId node,const signal_contracts::OwnedDataFrame& frame,std::optional<std::uint64_t> ordinal,std::shared_ptr<const void> owner,std::optional<std::uint64_t> filled_count){return impl_->local?impl_->local->feed_numeric_owned(node,frame,ordinal,std::move(owner),filled_count):signal_contracts::Status::unavailable;}
signal_contracts::Status NumericDesktopHost::query_numeric(const EndpointIdentity& endpoint,DeliveredNumericFrame& frame,std::uint64_t* ordinal,std::uint64_t* filled_count){return impl_->local?impl_->local->query_numeric(endpoint,frame,ordinal,filled_count):signal_contracts::Status::unavailable;}
bool NumericDesktopHost::listen(QHostAddress address,quint16 port,bool mutations){bool ok=false;impl_->call([&]{if(impl_->local){if(!impl_->server)impl_->server=std::make_unique<NumericControlServer>(*impl_->local);ok=impl_->server->listen(address,port,mutations);}});return ok;}
quint16 NumericDesktopHost::server_port()const{quint16 port=0;impl_->call([&]{if(impl_->server)port=impl_->server->port();});return port;}
void NumericDesktopHost::close_server(){impl_->call([&]{if(impl_->server)impl_->server->close();});}
QJsonObject NumericDesktopHost::routing_diagnostics()const{QJsonObject result;impl_->call([&]{if(impl_->routing)result=impl_->routing->diagnostics();});return result;}
}
