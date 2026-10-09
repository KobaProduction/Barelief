#include <control/numeric_routing_adapter.h>
#include <control/numeric_remote_endpoint.h>
#include <pipeline/numeric_engine.h>
#include <control/numeric_remote_support.h>
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
bool adapter_valid(const QJsonObject& a,QString& error){
    for(const auto key:{"provider_uuid","stream_id","host","device_identity"})if(a.contains(key)&&(!a[key].isString()||!text(a[key].toString()))){error=QString("Invalid route %1").arg(key);return false;}
    if(a.contains("provider_uuid")&&QUuid(a["provider_uuid"].toString()).isNull()){error="Invalid route provider_uuid";return false;}
    if(a.contains("port")&&(!integer(a["port"],65535)||!a["port"].toInt())){error="Invalid route port";return false;}
    if(a.contains("remote_endpoint")){EndpointIdentity e;if(!a["remote_endpoint"].isObject()||!endpoint(a["remote_endpoint"].toObject(),e)){error="Invalid route remote_endpoint";return false;}}
    return true;
}
bool parse(const QJsonArray& array,std::vector<Declaration>& out,QString& error){
    if(array.size()>int(NumericRoutingAdapter::MaxRoutes)){error="Too many numeric routes";return false;}
    std::vector<Declaration> next;std::set<int> ids;
    for(const auto value:array){if(!value.isObject()){error="Route must be an object";return false;}auto object=value.toObject();
        if(!integer(object["node_id"],INT_MAX)||!ids.insert(object["node_id"].toInt()).second){error="Invalid or duplicate route node_id";return false;}
        const auto node=object.take("node_id").toInt();if(!adapter_valid(object,error))return false;next.push_back({node,std::move(object)});}
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
    void unavailable(Route& r){
        if(r.unavailable||!running())return;r.unavailable=true;r.gap=true;
        auto c=r.contract.view();FrameStamp stamp;stamp.available=false;stamp.producer_epoch=r.producer;stamp.generation=r.generation;
        stamp.epoch=state.epoch;stamp.graph_revision=state.graph_revision;
        DataFrameView f;
        if(auto* n=std::get_if<NumericContract>(&c))f={stamp,NumericView{n}};
        else f={stamp,StructuredView{&std::get<StructuredContract>(c),{}}};
        if(r.ring){
            const auto limits=engine.capabilities().numeric->runtime.admission.frame;
            if(!r.markers)r.markers=std::make_shared<MarkerStorage>(limits.max_metadata_bytes);
            OwnedDataFrame owned;
            if(retain_frame(f,limits,r.markers->payload_pool,r.markers->metadata_pool,owned)==Status::ok)
                engine.feed_numeric_owned(r.declaration.node,owned,r.ordinal,r.markers);
        }else engine.feed_numeric(r.declaration.node,f);
    }
    bool peer_current(const Peer& p) const{return p.epoch&&(!p.incarnation||p.incarnation==p.epoch);}
    bool matches(Route& r,const Peer& p,const DeliveredNumericFrame& f){
        if(!running()||!peer_current(p)||!f.context||f.context->endpoint!=r.source||f.delivery.epoch!=p.epoch||f.delivery.graph_revision!=p.graph_revision)return false;
        const auto* port=remote_numeric::find_port(p.graph,r.source);
        return port&&same_contract(port->contract.view(),r.contract.view());
    }
    void delivered(const std::shared_ptr<Peer>& p,DeliveredNumericFrame f,std::optional<std::uint64_t> ordinal={},std::optional<std::uint64_t> filled_count={}){
        if(shutting_down)return;
        for(auto& [id,r]:routes)if(r->peer.lock()==p&&f.context&&f.context->endpoint==r->source){
            if(!matches(*r,*p,f)){++r->rejected;r->error="Route contract or delivery fence mismatch";unavailable(*r);continue;}
            if(f.status!=Status::ok||!f.frame){unavailable(*r);continue;}
            // Preserve producer stamps and discontinuity exactly. A transport
            // outage does not manufacture a new DSP generation or erase history.
            if(engine.feed_numeric_owned(id,f.frame,ordinal,f.context->retention_owner,filled_count)!=Status::ok){++r->rejected;continue;}
            const auto stamp=f.frame.view().stamp;r->sequence=stamp.sequence;r->producer=stamp.producer_epoch;r->generation=stamp.generation;
            r->unavailable=false;r->gap=false;++r->accepted;if(ordinal){r->ordinal=*ordinal;r->ring=true;}else {r->ordinal=0;r->ring=false;}r->error.clear();
        }
    }
    std::shared_ptr<Peer> make_peer(QString key,QString host,quint16 port,QString device,std::uint64_t incarnation){
        auto p=std::make_shared<Peer>();p->key=std::move(key);p->host=std::move(host);p->port=port;p->device=std::move(device);p->incarnation=incarnation;
        p->remote=std::make_unique<NumericRemoteEndpoint>(p->host,p->port);
        p->remote->configure_network({{"device_identity",p->device}});
        std::weak_ptr<Peer> weak=p;
        p->remote->set_snapshot_handler([this,weak](const auto& snapshot){if(shutting_down)return;auto p=weak.lock();if(!p)return;
            if(!snapshot.epoch){for(auto& [id,r]:routes)if(r->peer.lock()==p)unavailable(*r);p->epoch=0;p->query=false;p->query_again=false;return;}
            if(p->incarnation&&snapshot.epoch!=p->incarnation){for(auto& [id,r]:routes)if(r->peer.lock()==p)unavailable(*r);}
            if(snapshot.epoch!=p->epoch||snapshot.graph_revision!=p->graph_revision){
                domain::NumericProcessingGraph candidate;
                if(!snapshot.graph.isEmpty()&&!desktop::decode_numeric_graph(QJsonDocument(snapshot.graph).toJson(QJsonDocument::Compact),candidate))return;
                p->graph=std::move(candidate);p->epoch=snapshot.epoch;p->graph_revision=snapshot.graph_revision;
                p->query_again=true;
            }
        });
        p->remote->set_numeric_frame_handler([this,weak](auto f){if(auto p=weak.lock())delivered(p,std::move(f));});
        p->remote->set_numeric_history_handler([this,weak](const auto& row){auto p=weak.lock();if(!p||shutting_down)return;
            if(row.event==NumericHistoryEvent::Row){if(running())p->query_again=true;return;}
            for(auto& [id,r]:routes)if(r->peer.lock()==p&&row.compact.context&&row.compact.context->endpoint==r->source&&matches(*r,*p,row.compact))unavailable(*r);
        });
        p->remote->set_numeric_history_snapshot_handler([this,weak](auto f,std::uint64_t ordinal,std::uint64_t filled_count){if(auto p=weak.lock())delivered(p,std::move(f),ordinal,filled_count);});
        p->remote->set_reply_handler([this,weak](const auto& reply){if(shutting_down)return;auto p=weak.lock();if(p&&p->query&&reply.request_id==p->query_id)p->query=false;});
        p->remote->connect_remote();return p;
    }
    bool resolve(Route& r,QString& host,quint16& port,QString& device,std::uint64_t& incarnation,EndpointIdentity& source){
        const auto& a=r.declaration.adapter;device=a.value("device_identity").toString("numeric-desktop");incarnation=0;
        if(a.contains("provider_uuid")){
            if(!a.contains("stream_id")){r.error="Directory route requires stream_id";return false;}
            if(!registry){r.error="Directory is not configured";return false;}
            QString error;const auto metadata=registry->resolve_engine(QUuid(a["provider_uuid"].toString()),&error);
            if(metadata.isEmpty()){r.error=error;return false;}
            QJsonObject selected;for(const auto entry:metadata["streams"].toArray())if(entry.toObject()["id"]==a["stream_id"]){selected=entry.toObject();break;}
            if(selected.isEmpty()||!endpoint(selected,source)){r.error="Canonical stream binding unavailable";return false;}
            host=metadata["address"].toString();port=quint16(metadata["port"].toInt());incarnation=metadata["publisher_incarnation"].toString().toULongLong();
        }else {if(!a.contains("host")||!a.contains("port")||!a.contains("remote_endpoint")){r.error="Direct route requires host, port and remote_endpoint";return false;}
            host=a["host"].toString();port=quint16(a["port"].toInt());endpoint(a["remote_endpoint"].toObject(),source);}
        return true;
    }
    bool refresh(QString* error){
        if(refreshing||shutting_down)return false;refreshing=true;struct Reset{bool& flag;~Reset(){flag=false;}} reset{refreshing};
        state=engine.snapshot();
        if(state.epoch!=epoch||state.graph_revision!=revision){
            domain::NumericProcessingGraph next;if(!state.graph.isEmpty()&&!desktop::decode_numeric_graph(QJsonDocument(state.graph).toJson(QJsonDocument::Compact),next)){warning="Invalid applied numeric routing graph";if(error)*error=warning;return false;}
            graph=std::move(next);epoch=state.epoch;revision=state.graph_revision;
        }
        std::map<int,std::shared_ptr<Route>> next;
        for(const auto& d:explicit_config?explicit_routes:graph_routes){
            const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==d.node;});
            if(node==graph.nodes.end()||node->kind!="NetworkInput"||!node->enabled||node->outputs.size()!=1)continue;
            auto previous=routes.find(d.node);std::shared_ptr<Route> r;
            if(previous!=routes.end()&&previous->second->declaration.adapter==d.adapter&&same_contract(previous->second->contract.view(),node->outputs[0].contract.view()))r=previous->second;
            else {r=std::make_shared<Route>();r->declaration=d;r->contract=node->outputs[0].contract;}
            next.emplace(d.node,std::move(r));
        }
        for(auto& [id,previous]:routes){const auto replacement=next.find(id);
            if(replacement==next.end()||replacement->second!=previous)unavailable(*previous);}
        routes=std::move(next);std::map<QString,std::vector<EndpointIdentity>> desired;
        for(auto& [id,r]:routes){
            QString host,device;quint16 port=0;std::uint64_t incarnation=0;EndpointIdentity source;
            if(!resolve(*r,host,port,device,incarnation,source)){unavailable(*r);r->peer.reset();r->peer_key.clear();continue;}
            const auto key=QString::fromLatin1(QJsonDocument(QJsonArray{host,int(port),device,QString::number(incarnation)}).toJson(QJsonDocument::Compact).toHex());
            if(r->peer_key!=key||r->source!=source){unavailable(*r);r->peer.reset();r->peer_key=key;r->source=source;r->gap=true;r->ordinal=0;}
            auto p=peers.find(key);if(p==peers.end()){
                if(peers.size()>=MaxPeers){r->error="Numeric route peer limit exceeded";unavailable(*r);continue;}
                p=peers.emplace(key,make_peer(key,host,port,device,incarnation)).first;
            }
            r->peer=p->second;auto& endpoints=desired[key];if(std::find(endpoints.begin(),endpoints.end(),source)==endpoints.end())endpoints.push_back(source);
        }
        for(auto it=peers.begin();it!=peers.end();){auto p=it->second;auto wanted=desired.find(it->first);
            if(wanted==desired.end()){it=peers.erase(it);continue;}
            if(p->endpoints!=wanted->second){
                std::size_t histories=0;for(const auto& e:wanted->second){for(const auto& [id,r]:routes)if(r->peer.lock()==p&&r->source==e){const auto c=r->contract.view();const auto* n=std::get_if<NumericContract>(&c);histories+=n&&n->shape.rank==Rank::Matrix;break;}}
                if(histories>MaxHistoryEndpointsPerPeer){for(auto& [id,r]:routes)if(r->peer.lock()==p){r->error="Too many History route endpoints";unavailable(*r);r->peer.reset();}++it;continue;}
                p->endpoints=wanted->second;p->remote->subscribe_numeric_frames(p->endpoints);p->query_again=true;
            }
            const auto now=remote_numeric::Clock{}.now_ms();
            if(!p->remote->snapshot().epoch&&now-p->last_connect>=500){p->last_connect=now;p->remote->connect_remote();}
            if(running()&&peer_current(*p)&&p->query_again&&!p->query&&now-p->last_query>=50){p->query=true;p->query_again=false;p->last_query=now;
                EngineCommand command;command.kind=EngineCommand::Snapshot;p->query_id=p->remote->submit(command);}
            if(!running())for(auto& [id,r]:routes)if(r->peer.lock()==p)r->gap=true;
            ++it;
        }
        warning.clear();for(const auto& [id,r]:routes)if(!r->error.isEmpty()){warning=r->error;break;}
        if(error)*error=warning;return warning.isEmpty();
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
bool graph_declarations(const QJsonObject& graph,std::vector<Declaration>& declarations,QString* error){
    QJsonArray array;
    for(const auto value:graph.value("nodes").toArray()){const auto node=value.toObject();auto adapter=node.value("adapter");
        if(adapter.isUndefined())adapter=node.value("internal-data").toObject().value("adapter");
        if(adapter.isUndefined()||(adapter.isObject()&&adapter.toObject().isEmpty()))continue;
        if(!adapter.isObject()){if(error)*error="Node adapter must be an object";return false;}
        auto entry=adapter.toObject();entry["node_id"]=node["id"];array.append(entry);
    }
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
