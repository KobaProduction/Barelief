#include <control/numeric_routing_adapter.h>
#include <control/numeric_remote_endpoint.h>
#include <pipeline/numeric_engine.h>
#include <control/numeric_remote_support.h>
#include <domain/graph/processing_model.h>
#include <aggregator/aggregator_client.h>
#include <aggregator/capabilities.h>
#include <QTimer>
#include <QPointer>
#include <QUuid>
#include <QJsonArray>
#include <map>
#include <set>
#include <algorithm>
#include <climits>

namespace pipeline {
using namespace signal_contracts;
namespace {
bool integer(const QJsonValue& v,int maximum){return v.isDouble()&&v.toDouble()>=0&&v.toDouble()<=maximum&&v.toDouble()==v.toInt(-1);}
bool text(const QString& s){return !s.isEmpty()&&s.toUtf8().size()<=256&&!s.contains(QChar(0));}
bool endpoint(const QJsonObject& o,EndpointIdentity& out){
    const auto graph=o["graph_id"].toString(),direction=o["direction"].toString();
    if(!text(graph)||!integer(o["node_id"],INT_MAX)||!integer(o["port_index"],65535)||(direction!="input"&&direction!="output"))return false;
    out={graph.toStdString(),o["node_id"].toInt(),direction=="input"?domain::PortDirection::Input:domain::PortDirection::Output,std::uint16_t(o["port_index"].toInt())};return true;
}
struct Declaration {domain::NodeId node=-1;QJsonObject adapter;};
bool adapter_text_valid(const QJsonObject& adapter,QString& error) {
    for(const auto key:{"provider_uuid","stream_id","host","device_identity"}) {
        if(!adapter.contains(key) || (adapter[key].isString() && text(adapter[key].toString())))continue;
        error=QString("Invalid route %1").arg(key);
        return false;
    }
    return true;
}
bool adapter_valid(const QJsonObject& a,QString& error){
    if(!adapter_text_valid(a,error))return false;
    if(a.contains("provider_uuid")&&QUuid(a["provider_uuid"].toString()).isNull()){error="Invalid route provider_uuid";return false;}
    if(a.contains("port")&&(!integer(a["port"],65535)||!a["port"].toInt())){error="Invalid route port";return false;}
    if(a.contains("remote_endpoint")){EndpointIdentity e;if(!a["remote_endpoint"].isObject()||!endpoint(a["remote_endpoint"].toObject(),e)){error="Invalid route remote_endpoint";return false;}}
    return true;
}
bool parse_declaration(const QJsonValue& value,std::set<int>& ids,Declaration& out,QString& error) {
    if(!value.isObject()){error="Route must be an object";return false;}
    auto object=value.toObject();
    if(!integer(object["node_id"],INT_MAX) || !ids.insert(object["node_id"].toInt()).second) {
        error="Invalid or duplicate route node_id";
        return false;
    }
    const auto node=object.take("node_id").toInt();
    if(!adapter_valid(object,error))return false;
    out={node,std::move(object)};
    return true;
}
bool parse(const QJsonArray& array,std::vector<Declaration>& out,QString& error){
    if(array.size()>int(NumericRoutingAdapter::MaxRoutes)){error="Too many numeric routes";return false;}
    std::vector<Declaration> next;std::set<int> ids;
    for(const auto value:array) {
        Declaration declaration;
        if(!parse_declaration(value,ids,declaration,error))return false;
        next.push_back(std::move(declaration));
    }
    out=std::move(next);return true;
}
}
struct NumericRoutingAdapter::Impl {
    struct Peer;
    struct MarkerStorage {
        std::array<std::byte,2> payload{};std::vector<std::byte> metadata;
        std::array<BufferSlot,2> payload_slots{},metadata_slots{};
        BufferPool payload_pool,metadata_pool;
        explicit MarkerStorage(std::size_t bytes):metadata(2*bytes),payload_pool(payload,payload_slots,1),metadata_pool(metadata,metadata_slots,bytes){}
    };
    struct Route {
        Declaration declaration;domain::OwnedProcessingContract contract;EndpointIdentity source;
        std::weak_ptr<Peer> peer;QString peer_key,error;bool unavailable=true,gap=true,ring=false;
        std::uint64_t sequence=0,producer=0,generation=0,accepted=0,rejected=0,ordinal=0;
        std::shared_ptr<MarkerStorage> markers;
    };
    struct Peer {
        QString key,host,device;quint16 port=0;std::uint64_t incarnation=0;
        std::unique_ptr<NumericRemoteEndpoint> remote;
        std::vector<EndpointIdentity> endpoints;
        domain::NumericProcessingGraph graph;quint64 graph_revision=UINT64_MAX,epoch=0;
        bool query=false,query_again=false;quint64 query_id=0;std::uint64_t last_query=0,last_connect=0;
    };
    NumericEngine& engine;QPointer<AggregatorClient> registry;QObject* owner;QTimer timer;
    std::vector<Declaration> explicit_routes,graph_routes;bool explicit_config=false,shutting_down=false,refreshing=false;
    std::map<int,std::shared_ptr<Route>> routes;std::map<QString,std::shared_ptr<Peer>> peers;
    domain::NumericProcessingGraph graph;EngineSnapshot state;quint64 revision=UINT64_MAX,epoch=0;
    QString warning;
    Impl(NumericEngine& e,AggregatorClient* r,QObject* o):engine(e),registry(r),owner(o),timer(o){
        timer.setInterval(25);QObject::connect(&timer,&QTimer::timeout,owner,[this]{refresh(nullptr);});timer.start();
    }
    ~Impl(){shutting_down=true;timer.stop();peers.clear();}
    bool running() const{return state.statuses.value("analyzer").state==State::Running;}
    FrameStamp unavailable_stamp(const Route& r) const {
        FrameStamp stamp;
        stamp.available=false;stamp.producer_epoch=r.producer;stamp.generation=r.generation;
        stamp.epoch=state.epoch;stamp.graph_revision=state.graph_revision;
        return stamp;
    }
    void deliver_unavailable_history(Route& r,const DataFrameView& frame) {
        const auto limits=engine.capabilities().numeric->runtime.admission.frame;
        if(!r.markers)r.markers=std::make_shared<MarkerStorage>(limits.max_metadata_bytes);
        OwnedDataFrame owned;
        if(retain_frame(frame,limits,r.markers->payload_pool,r.markers->metadata_pool,owned)==Status::ok)
            engine.feed_numeric_owned(r.declaration.node,owned,r.ordinal,r.markers);
    }
    void unavailable(Route& r){
        if(r.unavailable||!running())return;
        r.unavailable=true;r.gap=true;
        auto c=r.contract.view();
        const auto stamp=unavailable_stamp(r);
        DataFrameView f;
        if(auto* n=std::get_if<NumericContract>(&c))f={stamp,NumericView{n}};
        else f={stamp,StructuredView{&std::get<StructuredContract>(c),{}}};
        if(r.ring)deliver_unavailable_history(r,f);
        else engine.feed_numeric(r.declaration.node,f);
    }
    bool peer_current(const Peer& p) const{return p.epoch&&(!p.incarnation||p.incarnation==p.epoch);}
    bool matches(Route& r,const Peer& p,const DeliveredNumericFrame& f){
        if(!running()||!peer_current(p)||!f.context||f.context->endpoint!=r.source||f.delivery.epoch!=p.epoch||f.delivery.graph_revision!=p.graph_revision)return false;
        const auto* port=remote_numeric::find_port(p.graph,r.source);
        return port&&same_contract(port->contract.view(),r.contract.view());
    }
    static void remember_delivery(Route& route,const FrameStamp& stamp,std::optional<std::uint64_t> ordinal) {
        route.sequence=stamp.sequence;
        route.producer=stamp.producer_epoch;
        route.generation=stamp.generation;
        route.unavailable=false;
        route.gap=false;
        ++route.accepted;
        route.ordinal=ordinal.value_or(0);
        route.ring=ordinal.has_value();
        route.error.clear();
    }
    void deliver_route(Route& route,const Peer& peer,const DeliveredNumericFrame& frame,
        std::optional<std::uint64_t> ordinal,std::optional<std::uint64_t> filled) {
        if(!matches(route,peer,frame)) {
            ++route.rejected;
            route.error="Route contract or delivery fence mismatch";
            unavailable(route);
            return;
        }
        if(frame.status!=Status::ok || !frame.frame){unavailable(route);return;}
        // Preserve producer stamps and discontinuity through transport outages.
        if(engine.feed_numeric_owned(route.declaration.node,frame.frame,ordinal,frame.context->retention_owner,filled)!=Status::ok) {
            ++route.rejected;
            return;
        }
        remember_delivery(route,frame.frame.view().stamp,ordinal);
    }
    void delivered(const std::shared_ptr<Peer>& p,DeliveredNumericFrame f,std::optional<std::uint64_t> ordinal={},std::optional<std::uint64_t> filled_count={}){
        if(shutting_down)return;
        for(auto& [id,r]:routes)
            if(r->peer.lock()==p && f.context && f.context->endpoint==r->source)deliver_route(*r,*p,f,ordinal,filled_count);
    }
    void peer_unavailable(const std::shared_ptr<Peer>& peer) {
        for(auto& [id,route]:routes)if(route->peer.lock()==peer)unavailable(*route);
    }
    void update_peer_graph(Peer& peer,const EngineSnapshot& snapshot) {
        if(snapshot.epoch==peer.epoch && snapshot.graph_revision==peer.graph_revision)return;
        domain::NumericProcessingGraph candidate;
        if(!snapshot.graph.isEmpty() && !desktop::decode_numeric_graph(QJsonDocument(snapshot.graph).toJson(QJsonDocument::Compact),candidate))return;
        peer.graph=std::move(candidate);
        peer.epoch=snapshot.epoch;
        peer.graph_revision=snapshot.graph_revision;
        peer.query_again=true;
    }
    void peer_snapshot(const std::shared_ptr<Peer>& peer,const EngineSnapshot& snapshot) {
        if(shutting_down)return;
        if(!snapshot.epoch) {
            peer_unavailable(peer);
            peer->epoch=0;
            peer->query=false;
            peer->query_again=false;
            return;
        }
        if(peer->incarnation && snapshot.epoch!=peer->incarnation)peer_unavailable(peer);
        update_peer_graph(*peer,snapshot);
    }
    void peer_history(const std::shared_ptr<Peer>& peer,const DeliveredNumericHistoryRow& row) {
        if(shutting_down)return;
        if(row.event==NumericHistoryEvent::Row){if(running())peer->query_again=true;return;}
        for(auto& [id,route]:routes)
            if(route->peer.lock()==peer && row.compact.context && row.compact.context->endpoint==route->source
                && matches(*route,*peer,row.compact))unavailable(*route);
    }
    void bind_peer_delivery(const std::shared_ptr<Peer>& peer) {
        const std::weak_ptr<Peer> weak=peer;
        peer->remote->set_numeric_frame_handler([this,weak](auto frame){if(auto p=weak.lock())delivered(p,std::move(frame));});
        peer->remote->set_numeric_history_handler([this,weak](const auto& row){if(auto p=weak.lock())peer_history(p,row);});
        peer->remote->set_numeric_history_snapshot_handler([this,weak](auto frame,std::uint64_t ordinal,std::uint64_t filled) {
            if(auto p=weak.lock())delivered(p,std::move(frame),ordinal,filled);
        });
    }
    void bind_peer_control(const std::shared_ptr<Peer>& peer) {
        const std::weak_ptr<Peer> weak=peer;
        peer->remote->set_snapshot_handler([this,weak](const auto& snapshot){if(auto p=weak.lock())peer_snapshot(p,snapshot);});
        peer->remote->set_reply_handler([this,weak](const auto& reply) {
            if(shutting_down)return;
            const auto p=weak.lock();
            if(p && p->query && reply.request_id==p->query_id)p->query=false;
        });
    }
    std::shared_ptr<Peer> make_peer(QString key,QString host,quint16 port,QString device,std::uint64_t incarnation){
        auto p=std::make_shared<Peer>();p->key=std::move(key);p->host=std::move(host);p->port=port;p->device=std::move(device);p->incarnation=incarnation;
        p->remote=std::make_unique<NumericRemoteEndpoint>(p->host,p->port);
        p->remote->configure_network({{"device_identity",p->device}});
        bind_peer_control(p);
        bind_peer_delivery(p);
        p->remote->connect_remote();return p;
    }
    QJsonObject directory_metadata(Route& route) const {
        const auto& adapter=route.declaration.adapter;
        if(!adapter.contains("stream_id")){route.error="Directory route requires stream_id";return {};}
        if(!registry){route.error="Directory is not configured";return {};}
        QString error;
        const auto metadata=registry->resolve_engine(QUuid(adapter["provider_uuid"].toString()),&error);
        if(metadata.isEmpty())route.error=error;
        return metadata;
    }
    static QJsonObject directory_stream(const QJsonObject& metadata,const QJsonValue& stream_id) {
        const auto streams=metadata.value("streams").toArray();
        for(const auto& entry:streams){
            const auto stream=entry.toObject();
            if(stream.value("id")==stream_id)return stream;
        }
        return {};
    }
    bool resolve_directory(Route& route,QString& host,quint16& port,std::uint64_t& incarnation,EndpointIdentity& source) {
        const auto metadata=directory_metadata(route);
        if(metadata.isEmpty())return false;
        const auto stream=directory_stream(metadata,route.declaration.adapter["stream_id"]);
        if(stream.isEmpty() || !endpoint(stream,source)){route.error="Canonical stream binding unavailable";return false;}
        host=metadata["address"].toString();
        port=quint16(metadata["port"].toInt());
        incarnation=metadata["publisher_incarnation"].toString().toULongLong();
        return true;
    }
    static bool resolve_direct(Route& route,QString& host,quint16& port,EndpointIdentity& source) {
        const auto& adapter=route.declaration.adapter;
        if(!adapter.contains("host") || !adapter.contains("port") || !adapter.contains("remote_endpoint")) {
            route.error="Direct route requires host, port and remote_endpoint";
            return false;
        }
        host=adapter["host"].toString();
        port=quint16(adapter["port"].toInt());
        endpoint(adapter["remote_endpoint"].toObject(),source);
        return true;
    }
    bool resolve(Route& r,QString& host,quint16& port,QString& device,std::uint64_t& incarnation,EndpointIdentity& source) {
        const auto& adapter=r.declaration.adapter;
        device=adapter.value("device_identity").toString("numeric-desktop");
        incarnation=0;
        if(adapter.contains("provider_uuid"))return resolve_directory(r,host,port,incarnation,source);
        return resolve_direct(r,host,port,source);
    }
    using RouteMap=std::map<int,std::shared_ptr<Route>>;
    using Subscriptions=std::map<QString,std::vector<EndpointIdentity>>;
    struct ResolvedRoute {
        QString host,device;
        quint16 port=0;
        std::uint64_t incarnation=0;
        EndpointIdentity source;
    };
    bool load_applied_graph(QString* error) {
        if(state.epoch==epoch && state.graph_revision==revision)return true;
        domain::NumericProcessingGraph next;
        if(!state.graph.isEmpty() && !desktop::decode_numeric_graph(QJsonDocument(state.graph).toJson(QJsonDocument::Compact),next)) {
            warning="Invalid applied numeric routing graph";
            if(error)*error=warning;
            return false;
        }
        graph=std::move(next);
        epoch=state.epoch;
        revision=state.graph_revision;
        return true;
    }
    std::shared_ptr<Route> retained_route(const Declaration& declaration,const domain::ProcessingPort& output) {
        const auto previous=routes.find(declaration.node);
        if(previous!=routes.end() && previous->second->declaration.adapter==declaration.adapter
            && same_contract(previous->second->contract.view(),output.contract.view()))return previous->second;
        auto route=std::make_shared<Route>();
        route->declaration=declaration;
        route->contract=output.contract;
        return route;
    }
    RouteMap collect_routes() {
        std::map<int,std::shared_ptr<Route>> next;
        for(const auto& d:explicit_config?explicit_routes:graph_routes){
            const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==d.node;});
            if(node==graph.nodes.end()||node->kind!="NetworkInput"||!node->enabled)continue;
            const auto outputs=domain::data_outputs(*node);
            if(outputs.size()!=1)continue;
            next.emplace(d.node,retained_route(d,outputs.front()));
        }
        return next;
    }
    void install_routes(RouteMap next) {
        for(auto& [id,previous]:routes){const auto replacement=next.find(id);
            if(replacement==next.end()||replacement->second!=previous)unavailable(*previous);}
        routes=std::move(next);
    }
    static QString route_peer_key(const ResolvedRoute& resolved) {
        return QString::fromLatin1(QJsonDocument(QJsonArray{resolved.host,int(resolved.port),resolved.device,
            QString::number(resolved.incarnation)}).toJson(QJsonDocument::Compact).toHex());
    }
    void change_route_peer(Route& route,const QString& key,const EndpointIdentity& source) {
        if(route.peer_key==key && route.source==source)return;
        unavailable(route);
        route.peer.reset();
        route.peer_key=key;
        route.source=source;
        route.gap=true;
        route.ordinal=0;
    }
    std::shared_ptr<Peer> acquire_route_peer(Route& route,const QString& key,const ResolvedRoute& resolved) {
        auto peer=peers.find(key);
        if(peer!=peers.end())return peer->second;
        if(peers.size()>=MaxPeers) {
            route.error="Numeric route peer limit exceeded";
            unavailable(route);
            return {};
        }
        return peers.emplace(key,make_peer(key,resolved.host,resolved.port,resolved.device,resolved.incarnation)).first->second;
    }
    std::shared_ptr<Peer> resolve_route_peer(Route& route,ResolvedRoute& resolved) {
        if(!resolve(route,resolved.host,resolved.port,resolved.device,resolved.incarnation,resolved.source)) {
            unavailable(route);
            route.peer.reset();
            route.peer_key.clear();
            return {};
        }
        const auto key=route_peer_key(resolved);
        change_route_peer(route,key,resolved.source);
        const auto peer=acquire_route_peer(route,key,resolved);
        if(peer)route.peer=peer;
        return peer;
    }
    Subscriptions desired_subscriptions() {
        Subscriptions desired;
        for(auto& [id,route]:routes) {
            ResolvedRoute resolved;
            const auto peer=resolve_route_peer(*route,resolved);
            if(!peer)continue;
            auto& endpoints=desired[peer->key];
            if(std::find(endpoints.begin(),endpoints.end(),resolved.source)==endpoints.end())endpoints.push_back(resolved.source);
        }
        return desired;
    }
    bool history_endpoint(const std::shared_ptr<Peer>& peer,const EndpointIdentity& endpoint) const {
        for(const auto& [id,route]:routes) {
            if(route->peer.lock()!=peer || route->source!=endpoint)continue;
            const auto contract=route->contract.view();
            const auto* numeric=std::get_if<NumericContract>(&contract);
            return numeric && numeric->shape.rank==Rank::Matrix;
        }
        return false;
    }
    void reject_history_subscriptions(const std::shared_ptr<Peer>& peer) {
        for(auto& [id,route]:routes) {
            if(route->peer.lock()!=peer)continue;
            route->error="Too many History route endpoints";
            unavailable(*route);
            route->peer.reset();
        }
    }
    bool subscribe_peer(const std::shared_ptr<Peer>& peer,const std::vector<EndpointIdentity>& wanted) {
        if(peer->endpoints==wanted)return true;
        const auto histories=std::count_if(wanted.begin(),wanted.end(),
            [&](const auto& endpoint){return history_endpoint(peer,endpoint);});
        if(std::size_t(histories)>MaxHistoryEndpointsPerPeer) {
            reject_history_subscriptions(peer);
            return false;
        }
        peer->endpoints=wanted;
        peer->remote->subscribe_numeric_frames(peer->endpoints);
        peer->query_again=true;
        return true;
    }
    void query_peer(const std::shared_ptr<Peer>& peer,std::uint64_t now) {
        if(running() && peer_current(*peer) && peer->query_again && !peer->query && now-peer->last_query>=50) {
            peer->query=true;
            peer->query_again=false;
            peer->last_query=now;
            EngineCommand command;
            command.kind=EngineCommand::Snapshot;
            peer->query_id=peer->remote->submit(command);
        }
    }
    void poll_peer(const std::shared_ptr<Peer>& peer) {
        const auto now=remote_numeric::Clock{}.now_ms();
        if(!peer->remote->snapshot().epoch && now-peer->last_connect>=500) {
            peer->last_connect=now;
            peer->remote->connect_remote();
        }
        query_peer(peer,now);
        if(!running())for(auto& [id,route]:routes)if(route->peer.lock()==peer)route->gap=true;
    }
    void refresh_peers(const Subscriptions& desired) {
        for(auto it=peers.begin();it!=peers.end();) {
            const auto peer=it->second;
            const auto wanted=desired.find(it->first);
            if(wanted==desired.end()){it=peers.erase(it);continue;}
            if(subscribe_peer(peer,wanted->second))poll_peer(peer);
            ++it;
        }
    }
    bool collect_warning(QString* error) {
        warning.clear();
        for(const auto& [id,route]:routes)if(!route->error.isEmpty()){warning=route->error;break;}
        if(error)*error=warning;
        return warning.isEmpty();
    }
    bool refresh(QString* error) {
        if(refreshing || shutting_down)return false;
        refreshing=true;
        struct Reset {bool& flag;~Reset(){flag=false;}} reset{refreshing};
        state=engine.snapshot();
        if(!load_applied_graph(error))return false;
        install_routes(collect_routes());
        const auto desired=desired_subscriptions();
        refresh_peers(desired);
        return collect_warning(error);
    }
};
NumericRoutingAdapter::NumericRoutingAdapter(NumericEngine& e,AggregatorClient* r,QObject* parent):QObject(parent),impl_(std::make_unique<Impl>(e,r,this)){}
NumericRoutingAdapter::~NumericRoutingAdapter()=default;
void NumericRoutingAdapter::set_registry(AggregatorClient* registry){impl_->registry=registry;impl_->refresh(nullptr);}
bool NumericRoutingAdapter::configure(const QJsonObject& config,QString* error){
    QString failure;std::vector<Declaration> next;const auto network=config.value("network").toObject();
    const bool supplied=network.contains("routes");if(supplied&&(!network["routes"].isArray()||!parse(network["routes"].toArray(),next,failure))){if(error)*error=failure.isEmpty()?"network.routes must be an array":failure;return false;}
    impl_->explicit_config=supplied;impl_->explicit_routes=std::move(next);impl_->refresh(nullptr);if(error)error->clear();return true;
}
namespace {
bool graph_route_entry(const QJsonObject& node,QJsonArray& entries,QString* error) {
    auto adapter=node.value("adapter");
    if(adapter.isUndefined())adapter=node.value("internal-data").toObject().value("adapter");
    if(adapter.isUndefined() || (adapter.isObject() && adapter.toObject().isEmpty()))return true;
    if(!adapter.isObject()) {
        if(error)*error="Node adapter must be an object";
        return false;
    }
    auto entry=adapter.toObject();
    entry["node_id"]=node["id"];
    entries.append(entry);
    return true;
}
bool collect_graph_routes(const QJsonObject& graph,QJsonArray& array,QString* error){
    const auto nodes=graph.value("nodes").toArray();
    for(const auto& value:nodes)if(!graph_route_entry(value.toObject(),array,error))return false;
    return true;
}
bool graph_declarations(const QJsonObject& graph,std::vector<Declaration>& declarations,QString* error){
    QJsonArray array;
    if(!collect_graph_routes(graph,array,error))return false;
    QString failure;std::vector<Declaration> next;if(!parse(array,next,failure)){if(error)*error=failure;return false;}
    declarations=std::move(next);if(error)error->clear();return true;
}
}
bool NumericRoutingAdapter::validate_graph_routes(const QJsonObject& graph,QString* error) const{std::vector<Declaration> next;return graph_declarations(graph,next,error);}
bool NumericRoutingAdapter::configure_graph_routes(const QJsonObject& graph,QString* error){std::vector<Declaration> next;if(!graph_declarations(graph,next,error))return false;impl_->graph_routes=std::move(next);impl_->refresh(nullptr);return true;}
bool NumericRoutingAdapter::refresh(QString* error){return impl_->refresh(error);}
QJsonObject NumericRoutingAdapter::diagnostics() const{
    QJsonArray rows;for(const auto& [id,r]:impl_->routes)rows.append(QJsonObject{{"node_id",id},{"available",!r->unavailable},{"accepted",QString::number(r->accepted)},
        {"rejected",QString::number(r->rejected)},{"history_ordinal",QString::number(r->ordinal)},{"error",r->error}});
    return {{"routes",rows},{"peer_count",int(impl_->peers.size())},{"error",impl_->warning}};
}
}
