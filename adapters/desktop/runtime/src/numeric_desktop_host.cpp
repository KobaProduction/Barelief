#include <control/numeric_desktop_host.h>
#include <desktop/processing_timer_policy.h>
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
 desktop::ProcessingTimerPolicy timer_policy;
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
 // Only endpoint service ages grow with subscription metadata, never frame
 // ownership. Prefer endpoints least recently offered to the GUI when a
 // producing cohort exceeds the fixed 32 owning slots.
 std::vector<std::uint64_t> preview_service_age;std::uint64_t preview_service_serial=0;
 NumericDeliveryMailbox<DeliveredNumericHistoryRow,8> histories;
 struct HistorySnapshot {DeliveredNumericFrame frame;std::uint64_t ordinal,filled_count;bool explicit_history=true;};
 NumericDeliveryMailbox<HistorySnapshot,8> history_snapshots;
 std::vector<EndpointIdentity> selected;
 struct ResumeBaseline {EndpointIdentity endpoint;std::uint64_t ordinal,producer_epoch,generation;};
 std::vector<ResumeBaseline> resumed_ordinals;
 bool history_resync=false,resync_inflight=false;quint64 resync_request=0;
 struct LocalSnapshotQuery {std::vector<EndpointIdentity> endpoints;std::uint64_t generation;std::size_t next=0;};
 std::shared_ptr<LocalSnapshotQuery> local_query;bool local_query_ready=false;
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
   std::fill(preview_service_age.begin(),preview_service_age.end(),0);preview_service_serial=0;
  }cache=std::move(value);
 }
 void publish(EngineSnapshot value){delivery_mode=endpoint->port_delivery_mode();std::lock_guard lock(mutex);update_cache_locked(std::move(value));schedule_locked();}
 void publish(EngineReply value){auto live=endpoint->snapshot();std::lock_guard lock(mutex);update_cache_locked(std::move(live));
  if(value.request_id==resync_request){resync_request=0;resync_inflight=false;schedule_locked();return;}
  const auto id=std::find(submitted.begin(),submitted.end(),value.request_id);if(id==submitted.end())return;*id=0;
  // submit reserves a completion slot before dispatch. Accepted request IDs are
  // never evicted, and internal transport requests do not consume GUI slots.
  replies.push(std::move(value));schedule_locked();}
 std::size_t selected_index(const EndpointIdentity& id) const {
  const auto at=std::lower_bound(selected.begin(),selected.end(),id);
  return at!=selected.end()&&*at==id?std::size_t(at-selected.begin()):selected.size();
 }
 void coalesce_preview(const EndpointIdentity& endpoint) {
  const auto count=frames.size();
  for(std::size_t i=0;i<count;++i){auto previous=frames.pop();
   if(previous&&(!previous->context||previous->context->endpoint!=endpoint))frames.push(std::move(*previous));}
 }
 bool reserve_preview_slot(std::size_t incoming) {
  if(frames.size()!=32)return true;
  std::size_t victim=0;std::uint64_t newest=0;
  for(std::size_t i=0;i<32;++i){auto previous=frames.pop();
   const auto index=previous&&previous->context?selected_index(previous->context->endpoint):selected.size();
   const auto age=index<preview_service_age.size()?preview_service_age[index]:UINT64_MAX;
   if(age>=newest){newest=age;victim=i;}if(previous)frames.push(std::move(*previous));}
  if(preview_service_age[incoming]>=newest)return false;
  for(std::size_t i=0;i<32;++i){auto previous=frames.pop();if(previous&&i!=victim)frames.push(std::move(*previous));}
  return true;
 }
 void publish(DeliveredNumericFrame value){std::lock_guard lock(mutex);
  if(!value.context)return;
  const auto incoming=selected_index(value.context->endpoint);if(incoming==selected.size())return;
  // Coalesce a represented endpoint to its newest value. Wider cohorts share
  // the same 32 leases: replace the most recently serviced queued endpoint
  // only for one which has waited longer. Fixed topology order cannot starve
  // the first or last ports across successive GUI drains.
  coalesce_preview(value.context->endpoint);
  if(!reserve_preview_slot(incoming))return;
  frames.push(std::move(value));schedule_locked();}
 void publish(DeliveredNumericHistoryRow value){std::lock_guard lock(mutex);if(histories.size()==8){histories.pop();history_resync=true;}histories.push(std::move(value));schedule_locked();}
 void publish_history_snapshot(DeliveredNumericFrame value,std::uint64_t ordinal,std::uint64_t filled_count){std::lock_guard lock(mutex);if(history_snapshots.size()==8)history_snapshots.pop();history_snapshots.push({std::move(value),ordinal,filled_count});schedule_locked();}
 // One worker job and one coalesced pending resync, irrespective of GUI rate.
 // Only owning frames cross back; borrowed processing views stay in query_numeric.
 void query_local_snapshots(std::shared_ptr<LocalSnapshotQuery> query){
  QMetaObject::invokeMethod(worker,[this,query=std::move(query)]{run_local_snapshot_batch(query);},Qt::QueuedConnection);
 }
 bool collect_local_snapshot(LocalSnapshotQuery& query) {
    const auto& selectedEndpoint=query.endpoints[query.next];
    DeliveredNumericFrame full;std::uint64_t ordinal=0,filled_count=0;bool explicit_history=false;
    const auto status=local->query_numeric(selectedEndpoint,full,&ordinal,&filled_count,&explicit_history);
    if(status!=signal_contracts::Status::ok&&status!=signal_contracts::Status::unavailable)return true;
    std::lock_guard lock(mutex);
    if(retired||query.generation!=data_generation)return false;
    if(!history_snapshots.push({std::move(full),ordinal,filled_count,explicit_history}))return false;
    schedule_locked();
    return true;
 }
 void run_local_snapshot_batch(std::shared_ptr<LocalSnapshotQuery> query) {
   bool cancelled=false;
   for(std::size_t batch=0;batch<8&&query->next<query->endpoints.size();++batch){
    {std::lock_guard lock(mutex);
     if(retired||query->generation!=data_generation){cancelled=true;break;}
     if(history_snapshots.size()==8)break;
    }
    if(!collect_local_snapshot(*query)){
     std::lock_guard lock(mutex);
     cancelled=retired||query->generation!=data_generation;
     break;
    }
    ++query->next;
   }
   std::lock_guard lock(mutex);
   if(cancelled)query->next=query->endpoints.size();
   local_query=query;local_query_ready=true;
   if(!retired)schedule_locked();
 }
 struct DrainHandlers {
  std::function<void(EngineSnapshot)> snapshot;
  std::function<void(EngineReply)> reply;
  std::function<void(DeliveredNumericFrame)> frame;
  std::function<void(DeliveredNumericHistoryRow)> history;
  std::function<void(DeliveredNumericFrame,std::uint64_t,std::uint64_t)> history_snapshot;
  std::uint64_t snapshot_version,reply_version,frame_version,history_version,history_snapshot_version;
 };
 DrainHandlers capture_handlers() const {
  return {snapshot_handler,reply_handler,frame_handler,history_handler,history_snapshot_handler,
          snapshot_generation,reply_generation,frame_generation,history_generation,history_snapshot_generation};
 }
 bool current_delivery(std::uint64_t generation,DeliveryStamp delivery) const {
  std::lock_guard lock(mutex);
  return generation==data_generation&&delivery.epoch==cache.epoch&&
         delivery.graph_revision==cache.graph_revision&&cache.statuses.value("analyzer").state!=State::Stopped;
 }
 bool current_history_snapshot(std::uint64_t generation,const HistorySnapshot& snapshot) const {
  std::lock_guard lock(mutex);
  return generation==data_generation&&snapshot.frame.delivery.epoch==cache.epoch&&
         snapshot.frame.delivery.graph_revision==cache.graph_revision&&cache.statuses.value("analyzer").state!=State::Stopped;
 }
 void restore_history_snapshot(HistorySnapshot snapshot,std::uint64_t generation,const DrainHandlers& handlers) {
  if(!current_history_snapshot(generation,snapshot)||!snapshot.frame.context||!snapshot.frame.frame)return;
  const auto value=snapshot.frame.frame.view();
  const auto* numeric=std::get_if<signal_contracts::NumericView>(&value.payload);
  if(!snapshot.explicit_history && (!numeric || !numeric->ring.capacity)){
   if(handlers.frame&&handlers.frame_version==frame_generation)handlers.frame(std::move(snapshot.frame));
   return;
  }
  const auto stamp=value.stamp;
  auto found=std::find_if(resumed_ordinals.begin(),resumed_ordinals.end(),[&](const auto& item){return item.endpoint==snapshot.frame.context->endpoint;});
  if(found!=resumed_ordinals.end()&&found->producer_epoch==stamp.producer_epoch&&found->generation==stamp.generation&&
     !stamp.discontinuity&&snapshot.ordinal<found->ordinal)return;
  ResumeBaseline baseline{snapshot.frame.context->endpoint,snapshot.ordinal,stamp.producer_epoch,stamp.generation};
  if(found==resumed_ordinals.end())resumed_ordinals.push_back(std::move(baseline));else *found=std::move(baseline);
  if(handlers.history_snapshot&&handlers.history_snapshot_version==history_snapshot_generation)
   handlers.history_snapshot(std::move(snapshot.frame),snapshot.ordinal,snapshot.filled_count);
 }
 void begin_history_resync(std::uint64_t& generation,bool& resume,std::vector<EndpointIdentity>& endpoints) {
  std::lock_guard lock(mutex);generation=data_generation;
  if(history_resync&&!resync_inflight){resume=true;history_resync=false;histories.clear();resync_inflight=true;
   if(local)endpoints=selected;}
 }
 void request_history_resync(bool resume,std::vector<EndpointIdentity> endpoints,std::uint64_t generation) {
  if(resume&&local)query_local_snapshots(std::make_shared<LocalSnapshotQuery>(LocalSnapshotQuery{std::move(endpoints),generation}));
  else if(resume)call([this]{const auto id=endpoint->submit(EngineCommand{});std::lock_guard lock(mutex);
   resync_request=id;if(!id)resync_inflight=false;});
 }
 struct DeliveryBatch {
  std::optional<EngineReply> reply;
  std::optional<DeliveredNumericFrame> frame;
  std::optional<DeliveredNumericHistoryRow> history;
  std::uint64_t generation=0;
 };
 void mark_preview_serviced(const DeliveredNumericFrame& frame) {
  if(!frame.context||frame.delivery.epoch!=cache.epoch||frame.delivery.graph_revision!=cache.graph_revision)return;
  const auto at=std::lower_bound(selected.begin(),selected.end(),frame.context->endpoint);
  if(at==selected.end()||*at!=frame.context->endpoint)return;
  if(preview_service_serial==UINT64_MAX){std::fill(preview_service_age.begin(),preview_service_age.end(),0);preview_service_serial=0;}
  preview_service_age[std::size_t(at-selected.begin())]=++preview_service_serial;
 }
 DeliveryBatch pop_delivery_batch() {
  std::lock_guard lock(mutex);DeliveryBatch batch;
  batch.reply=replies.pop();batch.frame=frames.pop();batch.history=histories.pop();batch.generation=data_generation;
  if(batch.reply){--outstanding;batch.reply->snapshot=cache;}
  if(batch.frame)mark_preview_serviced(*batch.frame);
  return batch;
 }
 void deliver_history_row(DeliveredNumericHistoryRow history,const DrainHandlers& handlers) {
  auto prior=std::find_if(resumed_ordinals.begin(),resumed_ordinals.end(),[&](const auto& item){return history.compact.context&&item.endpoint==history.compact.context->endpoint;});
  bool changed=history.event!=NumericHistoryEvent::Row;
  if(prior!=resumed_ordinals.end()&&history.compact.frame){const auto stamp=history.compact.frame.view().stamp;
   changed|=stamp.producer_epoch!=prior->producer_epoch||stamp.generation!=prior->generation||stamp.discontinuity;}
  if(changed&&prior!=resumed_ordinals.end()){resumed_ordinals.erase(prior);prior=resumed_ordinals.end();}
  if(history.event!=NumericHistoryEvent::Row||prior==resumed_ordinals.end()||history.accepted_row_ordinal>prior->ordinal)
   handlers.history(std::move(history));
 }
 bool deliver_batch(DeliveryBatch batch,const DrainHandlers& handlers,QPointer<NumericDesktopHost> guard) {
  if(batch.reply&&handlers.reply&&handlers.reply_version==reply_generation)handlers.reply(std::move(*batch.reply));
  if(!guard)return false;
  if(batch.frame&&handlers.frame&&handlers.frame_version==frame_generation&&current_delivery(batch.generation,batch.frame->delivery))
   handlers.frame(std::move(*batch.frame));
  if(!guard)return false;
  if(batch.history&&handlers.history&&handlers.history_version==history_generation&&
     current_delivery(batch.generation,batch.history->compact.delivery))deliver_history_row(std::move(*batch.history),handlers);
  return bool(guard);
 }
 void continue_local_query() {
  std::shared_ptr<LocalSnapshotQuery> continuation;
  {std::lock_guard lock(mutex);if(local_query_ready){local_query_ready=false;
    if(local_query->next==local_query->endpoints.size()){local_query.reset();resync_inflight=false;if(history_resync)schedule_locked();}
    else continuation=local_query;}}
  if(continuation)query_local_snapshots(std::move(continuation));
 }
 void drain(){QPointer<NumericDesktopHost> guard(&owner);const auto handlers=capture_handlers();
  EngineSnapshot state;{std::lock_guard lock(mutex);pending=false;state=cache;}
  if(handlers.snapshot&&handlers.snapshot_version==snapshot_generation)handlers.snapshot(state);if(!guard)return;
  std::uint64_t resumeGeneration;bool resume=false;std::vector<EndpointIdentity> endpoints;
  begin_history_resync(resumeGeneration,resume,endpoints);
  if(resumed_generation!=resumeGeneration){resumed_generation=resumeGeneration;resumed_ordinals.clear();}
  request_history_resync(resume,std::move(endpoints),resumeGeneration);
  for(std::size_t i=0;i<8;++i){std::optional<HistorySnapshot> full;{std::lock_guard lock(mutex);full=history_snapshots.pop();}
   if(!full)break;restore_history_snapshot(std::move(*full),resumeGeneration,handlers);if(!guard)return;}
  for(std::size_t i=0;i<128;++i){auto batch=pop_delivery_batch();
   if(!batch.reply&&!batch.frame&&!batch.history)break;
   if(!deliver_batch(std::move(batch),handlers,guard))return;}
  // A worker batch cannot overwrite other selected endpoints while the GUI is
  // paused. Advance only after this bounded owning batch has been consumed.
  continue_local_query();
 }
 void initialize_remote(QString host,quint16 port,const std::optional<desktop::SerialEndpoint>& serial) {
  if(serial)endpoint=std::make_unique<NumericRemoteEndpoint>(*serial);
  else endpoint=std::make_unique<NumericRemoteEndpoint>(std::move(host),port);
  static_cast<NumericRemoteEndpoint*>(endpoint.get())->set_numeric_history_snapshot_handler(
   [this](auto frame,std::uint64_t ordinal,std::uint64_t filled_count){publish_history_snapshot(std::move(frame),ordinal,filled_count);});
 }
 void initialize_local(AudioSourceFactory factory) {
  auto engine=std::make_unique<NumericEngine>(std::move(factory));local=engine.get();
  registry=std::make_unique<AggregatorClient>(AggregatorClientConfig{QUuid::createUuid(),ComponentType::data_sink,0});
  auto next_routing=std::make_unique<NumericRoutingAdapter>(*engine,registry.get());
  routing=next_routing.get();auto* routes=routing;
  engine->set_adapter_handlers(std::move(next_routing),
   {[routes](const QJsonObject& graph,QString* error){return routes->validate_graph_routes(graph,error);},
    [routes](const QJsonObject& graph){routes->configure_graph_routes(graph);},
    [routes](const QJsonObject& config,QString* error){return routes->configure(config,error);},
    [routes]{return routes->diagnostics().value("error").toString();}});
  endpoint=std::move(engine);
 }
 void bind_endpoint_handlers() {
  endpoint->set_snapshot_handler([this](auto value){publish(std::move(value));});
  endpoint->set_reply_handler([this](auto value){publish(std::move(value));});
  endpoint->set_numeric_frame_handler([this](auto value){publish(std::move(value));});
  endpoint->set_numeric_history_handler([this](auto value){publish(std::move(value));});
  publish(endpoint->snapshot());
 }
 bool subscribe_frames(std::vector<EndpointIdentity> endpoints,QString& error) {
  {std::lock_guard lock(mutex);if(selected==endpoints)return true;}
  if(!endpoint->subscribe_numeric_frames(endpoints,&error))return false;
  std::lock_guard lock(mutex);
  selected=std::move(endpoints);preview_service_age.assign(selected.size(),0);preview_service_serial=0;
  ++data_generation;frames.clear();histories.clear();history_snapshots.clear();history_resync=!selected.empty();
  schedule_locked();return true;
 }
 ~Impl(){ {std::lock_guard lock(mutex);retired=true;}call([this]{server.reset();endpoint.reset();registry.reset();routing=nullptr;local=nullptr;});thread.quit();thread.wait();}
};
NumericDesktopHost::NumericDesktopHost(AudioSourceFactory factory,QString host,quint16 port,std::optional<desktop::SerialEndpoint> serial,QObject* parent):QObject(parent),impl_(std::make_unique<Impl>(*this)){
 impl_->call([&,factory=std::move(factory),host=std::move(host),serial=std::move(serial)]()mutable{
  if(!host.isEmpty())impl_->initialize_remote(std::move(host),port,serial);
  else impl_->initialize_local(std::move(factory));
  impl_->bind_endpoint_handlers();
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
void NumericDesktopHost::set_numeric_preview_cadence_hz(int hz){impl_->call([this,hz]{impl_->endpoint->set_numeric_preview_cadence_hz(hz);});}
EngineSnapshot NumericDesktopHost::snapshot()const{std::lock_guard lock(impl_->mutex);return impl_->cache;}
bool NumericDesktopHost::configure_network(QJsonObject config,QString* error){bool ok=false;QString e;impl_->call([&]{ok=impl_->endpoint->configure_network(std::move(config),&e);});if(error)*error=e;return ok;}
QString NumericDesktopHost::network_warning()const{QString value;impl_->call([&]{value=impl_->endpoint->network_warning();});return value;}
void NumericDesktopHost::connect_remote(){impl_->call([&]{impl_->endpoint->connect_remote();});}
void NumericDesktopHost::disconnect_remote(){impl_->call([&]{impl_->endpoint->disconnect_remote();impl_->publish(impl_->endpoint->snapshot());});}
EnginePort::PortDeliveryMode NumericDesktopHost::port_delivery_mode()const noexcept{return impl_->delivery_mode.load();}
bool NumericDesktopHost::subscribe_numeric_frames(std::vector<EndpointIdentity> endpoints,QString* error){
 std::sort(endpoints.begin(),endpoints.end());endpoints.erase(std::unique(endpoints.begin(),endpoints.end()),endpoints.end());
 bool ok=false;QString e;impl_->call([&]{ok=impl_->subscribe_frames(std::move(endpoints),e);});
 if(error)*error=e;return ok;
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
