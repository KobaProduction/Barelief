#include "synthetic_capture.h"
#include "pipeline/engine_client.h"
#include "pipeline/network_runtime.h"
#include "pipeline/stream_transport.h"
#include "pipeline/port_delivery_codec.h"
#include "core/network/tcp_packet_socket.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonArray>
#include <QDataStream>
#include <QJsonDocument>
#include <QTcpServer>
#include <stdexcept>
#include <iostream>
using namespace pipeline;
static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static bool wait(std::function<bool()> predicate) {
    QElapsedTimer t;
    t.start();
    while (!predicate() && t.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}
static void settle_packets(){QElapsedTimer elapsed;elapsed.start();wait([&]{return elapsed.elapsed()>=40;});}
class DeliveryPeer final : public QTcpServer {
public:
    TcpBDSPSocket *peer=nullptr;uint64_t session=42,revision=1,status_revision=0,nonce=0,sequence=0;
    void status(State state) {
        QByteArray bytes;QDataStream out(&bytes,QIODevice::WriteOnly);out.setVersion(QDataStream::Qt_6_0);out.setByteOrder(QDataStream::LittleEndian);out.setFloatingPointPrecision(QDataStream::SinglePrecision);
        out<<quint16(1)<<quint64(session)<<quint64(++status_revision)<<quint64(revision)<<quint64(99)<<quint64(0)
           <<QJsonDocument(QJsonObject{{"graph_id","main"}}).toJson(QJsonDocument::Compact)<<quint32(1)
           <<QString("Analyzer")<<quint8(state)<<QString{}<<quint64(status_revision)<<false;
        require(peer->send_packet(7,bytes),"fixture status send");
    }
    void legacy(std::vector<std::pair<int,TypedFrame>> values,uint64_t ep,uint64_t rev) {
        QByteArray bytes;auto put=[&](uint64_t value,int n){for(int i=0;i<n;++i)bytes.append(char(value>>(i*8)));};
        put(2,2);put(ep,8);put(rev,8);put(values.size(),4);
        for(auto &[id,value]:values){value.epoch=ep;value.graph_revision=rev;const auto payload=stream::encode(value);put(id,4);put(payload.size(),4);bytes.append(reinterpret_cast<const char*>(payload.data()),payload.size());}
        require(peer->send_packet(17,bytes),"fixture legacy send");
    }
    void batch(port_delivery::Frames value) {
        value.nonce=nonce;value.session_epoch=session;value.graph_id="main";
        std::array<uint8_t,4096> storage{};const auto encoded=port_delivery::encode(value,storage);require(bool(encoded),"batch encode");
        require(peer->send_packet(20,QByteArray(reinterpret_cast<const char*>(storage.data()),encoded.written_bytes)),"batch send");
    }
    void canonical(std::vector<TypedFrame> values,uint64_t local_epoch,uint64_t local_revision,bool snapshot=false,uint64_t session_override=0) {
        port_delivery::Frames batch;batch.snapshot=snapshot;batch.nonce=nonce;batch.session_epoch=session_override?session_override:session;batch.sequence=++sequence;batch.graph_id="main";batch.delivery={local_revision,local_epoch};
        int node=1;for(auto &value:values){DeliveredPortFrame port;port.endpoint={"main",node++,PortDirection::Input,0};port.delivery=batch.delivery;port.kind=domain::NodeKind::MelView;port.frame=std::move(value);port.contract.type=FrameType::Mel;port.contract.sample_rate=48000;port.contract.columns=1;port.contract_known=true;batch.entries.push_back(std::move(port));}
        std::array<uint8_t,4096> storage{};auto result=port_delivery::encode(batch,storage);require(bool(result),"fixture canonical grammar");require(peer->send_packet(20,QByteArray(reinterpret_cast<const char*>(storage.data()),result.written_bytes)),"fixture canonical send");
    }
protected:
    void incomingConnection(qintptr descriptor) override {
        peer=new TcpBDSPSocket(this);require(peer->setSocketDescriptor(descriptor),"fixture descriptor");
        QObject::connect(peer,&TcpBDSPSocket::packetReceived,this,[&](quint8 id,QByteArray bytes){if(id!=18)return;
            port_delivery::Subscribe request;require(bool(port_delivery::decode({reinterpret_cast<const uint8_t*>(bytes.constData()),size_t(bytes.size())},request)),"fixture subscription");nonce=request.nonce;
            port_delivery::Ack ack;ack.nonce=nonce;ack.session_epoch=session;ack.graph_revision=revision;ack.graph_id="main";ack.limits=request.limits;ack.type_mask=request.type_mask;
            std::array<uint8_t,1024> storage{};const auto result=port_delivery::encode(ack,storage);require(bool(result),"fixture ACK");peer->send_packet(19,QByteArray(reinterpret_cast<const char*>(storage.data()),result.written_bytes));
        });status(State::Running);
    }
};
static void stopped_delivery_fences() {
    DeliveryPeer server;require(server.listen(QHostAddress::LocalHost,0),"delivery peer listen");EngineClient client("127.0.0.1",server.serverPort());
    require(wait([&]{return client.snapshot().epoch==42;}),"fixture handshake");
    int legacy_count=0,canonical_count=0;TypedFrame last;
    client.set_view_handler([&](ViewFrame view){++legacy_count;last=view.views.value(1);});
    client.set_port_frame_handler([&](DeliveredPortFrame value){++canonical_count;last=value.frame;});
    TypedFrame data;data.type=FrameType::Mel;data.sample_rate=48000;data.sequence=1;data.generation=99;data.epoch=800;data.graph_revision=900;data.values={.5f};
    TypedFrame clear;clear.type=FrameType::Mel;clear.available=false;
    server.legacy({{1,data}},42,1);require(wait([&]{return legacy_count==1;}),"running legacy delivery");
    server.status(State::Stopped);require(wait([&]{return client.snapshot().statuses.value("Analyzer").state==State::Stopped;}),"stopped status");
    server.legacy({{1,data}},42,1);server.legacy({{1,clear},{2,data}},42,1);settle_packets();require(legacy_count==1,"Stopped accepted available or mixed legacy snapshot");
    server.legacy({{1,clear}},41,1);server.legacy({{1,clear}},42,0);settle_packets();require(legacy_count==1,"Stopped accepted stale legacy clear");
    server.legacy({{1,clear}},42,1);require(wait([&]{return legacy_count==2 && !last.available;}),"Stopped rejected current all-unavailable legacy clear");
    EngineCommand subscription;subscription.kind=EngineCommand::Subscribe;subscription.subscriptions=0;client.submit(subscription);
    server.legacy({{1,clear}},42,1);settle_packets();require(legacy_count==2,"Unsubscribed legacy clear delivered");
    subscription.subscriptions=EngineCommand::Views|EngineCommand::TypedViews;client.submit(subscription);server.revision=2;server.status(State::Running);
    require(wait([&]{return client.snapshot().graph_revision==2;}),"new revision");server.legacy({{1,data}},42,2);require(wait([&]{return legacy_count==3 && last.available;}),"new legacy data");
    server.legacy({{1,clear}},42,1);server.legacy({{1,clear}},41,2);settle_packets();require(legacy_count==3 && last.available,"Old legacy clear erased new Running/Apply data");
    client.disconnect_remote();require(wait([&]{return client.snapshot().epoch==0;}),"fixture disconnect");server.session=43;client.connect_remote();
    require(wait([&]{return client.snapshot().epoch==43;}),"fixture reconnect");server.legacy({{1,data}},43,2);require(wait([&]{return legacy_count==4 && last.available;}),"reconnected data");
    server.legacy({{1,clear}},42,2);settle_packets();require(legacy_count==4 && last.available,"Prior-session legacy clear erased reconnected frame");
    require(client.subscribe_port_frames({}),"canonical fixture subscribe");require(wait([&]{return client.port_delivery_mode()==EnginePort::PortDeliveryMode::CanonicalPortsV1;}),"canonical negotiation");
    server.canonical({data},10,2,true);require(wait([&]{return canonical_count==1;}),"canonical initial snapshot");require(last.epoch==800 && last.graph_revision==900,"Canonical fence replaced producer metadata");
    server.status(State::Stopped);require(wait([&]{return client.snapshot().statuses.value("Analyzer").state==State::Stopped;}),"canonical Stop");
    server.canonical({data},999,2);server.canonical({clear,data},1000,2);settle_packets();require(canonical_count==1,"Stopped accepted canonical available/mixed snapshot");
    server.canonical({clear},11,2);require(wait([&]{return canonical_count==2 && !last.available;}),"canonical unavailable clear");
    server.revision=3;server.status(State::Running);require(wait([&]{return client.snapshot().graph_revision==3;}),"canonical Apply");
    server.canonical({data},12,3);require(wait([&]{return canonical_count==3 && last.available;}),"canonical fresh data");
    server.canonical({clear},11,2);server.canonical({clear},12,3,false,41);settle_packets();require(canonical_count==3 && last.available,"Stale canonical clear erased new frame");
    client.disconnect_remote();
}
static void complete_coverage() {
    DeliveryPeer server;require(server.listen(QHostAddress::LocalHost,0),"coverage listen");EngineClient client("127.0.0.1",server.serverPort());
    require(wait([&]{return client.snapshot().epoch==42;}),"coverage handshake");
    const EndpointIdentity a{"main",1,PortDirection::Output,0},b{"main",1,PortDirection::Output,1};
    require(client.subscribe_port_frames({a,b}),"coverage subscribe");require(wait([&]{return client.port_delivery_mode()==EnginePort::PortDeliveryMode::CanonicalPortsV1;}),"coverage ACK");
    port_delivery::Frames full;full.snapshot=true;full.delivery={1,10};full.sequence=1;
    DeliveredPortFrame first;first.endpoint=a;first.kind=domain::NodeKind::Spectrogram;first.delivery=full.delivery;first.frame.type=FrameType::History;first.frame.available=false;first.contract.type=FrameType::History;
    auto second=first;second.endpoint=b;second.frame.type=FrameType::Scalar;second.contract.type=FrameType::Scalar;
    full.entries={first,second};
    auto invalid=full;invalid.sequence=100;invalid.delivery={100,100};invalid.entries={first};invalid.entries[0].delivery=invalid.delivery;server.batch(invalid);settle_packets();require(client.snapshot().port_frames.empty(),"partial initial accepted");
    server.batch(full);require(wait([&]{return client.snapshot().port_frames.size()==2;}),"valid complete poisoned by rejected future fences");
    auto preserved=[&]{settle_packets();const auto current=client.snapshot().port_frames;require(current.size()==2 && current[0].delivery==full.delivery && current[1].frame.type==FrameType::Scalar,"invalid batch changed accepted cache");};
    invalid=full;invalid.sequence=100;invalid.entries.clear();server.batch(invalid);preserved();
    invalid=full;invalid.sequence=100;invalid.entries[1].endpoint.node=99;server.batch(invalid);preserved();
    invalid=full;invalid.sequence=100;invalid.entries[1].endpoint.direction=PortDirection::Input;server.batch(invalid);preserved();
    invalid=full;invalid.sequence=100;invalid.entries[1].frame.type=FrameType::Mel;invalid.entries[1].contract.type=FrameType::Mel;server.batch(invalid);preserved();
    auto duplicate=full;duplicate.nonce=server.nonce;duplicate.session_epoch=server.session;duplicate.graph_id="main";duplicate.sequence=100;
    std::array<uint8_t,4096> bytes{};const auto encoded=port_delivery::encode(duplicate,bytes);require(bool(encoded),"duplicate fixture encode");
    const size_t first_row=44+2+4+2;const size_t second_row=first_row+40+domain::kind_id(first.kind).size()+stream::payload_size(first.frame);
    bytes[second_row+5]=0;bytes[second_row+6]=0;
    server.peer->send_packet(20,QByteArray(reinterpret_cast<const char*>(bytes.data()),encoded.written_bytes));preserved();
    full.sequence=2;full.delivery.epoch=11;for(auto &entry:full.entries)entry.delivery=full.delivery;server.batch(full);
    require(wait([&]{return client.snapshot().port_frames[0].delivery.epoch==11;}),"valid unavailable complete poisoned by invalid sequence");
    client.disconnect_remote();
}
static void server_stop_start_queue() {
    AudioPipeline engine(test_fixture::synthetic_capture_factory());QString error;
    const QJsonObject graph{{"version",2},{"nodes",QJsonArray{
        QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","mel"},{"stream_id","fixture"}}}},
        QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","MelView"}}}}}},
        {"connections",QJsonArray{QJsonObject{{"outNodeId",0},{"outPortIndex",0},{"inNodeId",1},{"inPortIndex",0}}}}};
    const auto applied=engine.apply_graph(graph,error);require(applied,qPrintable(error));engine.start_analyzer();EngineServer server(engine);require(server.listen(QHostAddress::LocalHost,0),"queue fixture listen");
    EngineClient client("127.0.0.1",server.port());require(wait([&]{return client.snapshot().statuses.value("Analyzer").state==State::Running;}),"queue fixture handshake");
    uint64_t legacy_sequence=0,canonical_sequence=0;bool resume=false,stale_clear=false;
    client.set_view_handler([&](ViewFrame view){if(!view.views.contains(1))return;const auto value=view.views[1];if(value.available)legacy_sequence=value.sequence;else stale_clear|=resume;});
    client.set_port_frame_handler([&](DeliveredPortFrame value){if(value.frame.available)canonical_sequence=value.frame.sequence;else stale_clear|=resume;});
    require(client.subscribe_port_frames({{"main",1,PortDirection::Input,0}}),"queue fixture subscription");require(wait([&]{return client.port_delivery_mode()==EnginePort::PortDeliveryMode::CanonicalPortsV1;}),"queue fixture ACK");
    TypedFrame frame;frame.type=FrameType::Mel;frame.sample_rate=48000;frame.generation=99;frame.epoch=800;frame.producer_epoch=801;frame.graph_revision=900;frame.sequence=1;frame.values={.5f};
    engine.feed_typed(0,frame);require(wait([&]{return legacy_sequence==1 && canonical_sequence==1;}),"queue fixture first data");
    resume=true;engine.stop_analyzer();engine.start_analyzer();frame.sequence=2;engine.feed_typed(0,frame);
    require(wait([&]{return legacy_sequence==2 && canonical_sequence==2;}),"queue fixture resumed data");settle_packets();
    require(!stale_clear,"Queued Stop clear erased same-revision resumed data");
}
static void canonical_protocol() {
    AudioPipeline engine(test_fixture::synthetic_capture_factory());QString error;
    const QJsonObject graph{{"version",2},{"nodes",QJsonArray{
        QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","scalar"},{"stream_id","fixture"}}}},
        QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","ScalarView"}}}}}},
        {"connections",QJsonArray{QJsonObject{{"outNodeId",0},{"outPortIndex",0},{"inNodeId",1},{"inPortIndex",0}}}}};
    require(engine.apply_graph(graph,error),qPrintable(error));EngineServer server(engine);require(server.listen(),"canonical fixture listen");
    TcpBDSPSocket peer;std::vector<port_delivery::Ack> acks;std::vector<port_delivery::Frames> frames;bool refused=false;
    QObject::connect(&peer,&TcpBDSPSocket::packetReceived,&peer,[&](quint8 id,QByteArray bytes){
        const auto view=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.constData()),size_t(bytes.size()));
        if(id==19){port_delivery::Ack value;require(bool(port_delivery::decode(view,value)),"ACK grammar");acks.push_back(value);}
        if(id==20){port_delivery::Frames value;require(bool(port_delivery::decode(view,value)),"frames grammar");frames.push_back(value);}
        if(id==14)refused=true;
    });
    peer.connectToHost(QHostAddress::LocalHost,server.port());require(wait([&]{return peer.state()==QAbstractSocket::ConnectedState;}),"fixture connection");
    port_delivery::Subscribe request;request.nonce=1;request.endpoints={{"main",1,PortDirection::Input,0}};
    auto send=[&]{std::array<uint8_t,2048> storage{};const auto result=port_delivery::encode(request,storage);require(bool(result),"request grammar");require(peer.send_packet(18,QByteArray(reinterpret_cast<const char*>(storage.data()),result.written_bytes)),"request send");};
    send();require(wait([&]{return acks.size()==1 && !frames.empty();}),"canonical ACK and initial complete snapshot");
    require(acks[0].status==port_delivery::Status::Accepted && frames[0].snapshot && frames[0].entries.size()==1 && frames[0].entries[0].endpoint==request.endpoints[0],"canonical first snapshot identity");
    const auto nonce=frames[0].nonce;
    request.nonce=2;request.endpoints[0].node=99;send();require(wait([&]{return acks.size()==2;}),"invalid endpoint refusal");
    require(acks.back().status==port_delivery::Status::InvalidEndpoint,"invalid endpoint accepted");
    engine.start_analyzer();TypedFrame value;value.type=FrameType::Scalar;value.values={0};value.sequence=1;value.sample_rate=48000;value.graph_revision=800;value.epoch=900;
    require(wait([&]{engine.feed_typed(0,value);return std::any_of(frames.begin(),frames.end(),[&](const auto &f){return f.nonce==nonce && !f.snapshot && f.entries[0].frame.available;});}),"refused replacement destroyed accepted subscription");
    const auto delivered=frames.back().entries[0];require(delivered.frame.graph_revision==800 && delivered.frame.epoch==900 && delivered.delivery.graph_revision==engine.graph_revision(),"producer metadata overwritten by local fence");
    const auto before_removal=frames.size();auto removed=graph;auto nodes=removed["nodes"].toArray();nodes.removeAt(1);removed["nodes"]=nodes;removed["connections"]=QJsonArray{};
    require(engine.apply_graph(removed,error),qPrintable(error));
    require(wait([&]{return frames.size()>before_removal && frames.back().snapshot && frames.back().entries.size()==1 && frames.back().entries[0].endpoint==EndpointIdentity{"main",1,PortDirection::Input,0} && !frames.back().entries[0].frame.available;}),"removed selected endpoint omitted from complete snapshot");
    send();require(wait([&]{return refused;}),"replayed nonce not refused");peer.abort();engine.stop_analyzer();
}
#include "aggregator/capabilities.h"
#include "aggregator/aggregator_client.h"
#include "aggregator/aggregator_server.h"
#include <bdsp_protocol/stream_fragments.h>
class IncarnationStream final:public QTcpServer {
public: TcpBDSPSocket *peer=nullptr;int subscriptions=0;
 void send(const TypedFrame &frame){auto bytes=stream::encode(frame);std::array<uint8_t,60008> fragment{};auto r=bdsp_protocol::encode_fragment(bytes,0,fragment);require(bool(r),"fragment");require(peer->send_packet(13,QByteArray(reinterpret_cast<const char*>(fragment.data()),r.written_bytes)),"DATA send");}
protected: void incomingConnection(qintptr fd)override{peer=new TcpBDSPSocket(this);require(peer->setSocketDescriptor(fd),"raw fd");QObject::connect(peer,&TcpBDSPSocket::packetReceived,this,[this](quint8 id,QByteArray b){if(id==11){++subscriptions;peer->send_packet(12,b);}});}
};
static void registry_incarnation() {
 AggregatorServer directory;require(directory.start(0),"registry listen");IncarnationStream server;require(server.listen(QHostAddress::LocalHost,0),"stream listen");const auto provider=QUuid::createUuid();AggregatorClient advertiser({provider,ComponentType::core,0,321});
 auto caps=[&](const char *inc){return QJsonObject{{"stream_port",server.serverPort()},{"stream_delivery_features",QJsonArray{"port-identity-v1","optional-future"}},{"stream_protocol_versions",QJsonArray{2,3}},{"publisher_incarnation",inc},{"stream_limits",QJsonObject{{"max_frame_bytes",256},{"max_stream_id_bytes",8},{"max_streams",1}}},{"streams",QJsonArray{QJsonObject{{"id","chosen"},{"data_type","scalar"},{"graph_id","producer"},{"node_id",3},{"direction","input"},{"port_index",0},{"payload_version",3},{"subscription_version",3}}}}};};
 advertiser.set_capabilities(caps("77"));advertiser.connect_to_aggregator_server(QHostAddress::LocalHost,directory.serverPort());require(wait([&]{return advertiser.is_registered();}),"advertiser registered");
 AggregatorClient observer({QUuid::createUuid(),ComponentType::core,0,322});observer.connect_to_aggregator_server(QHostAddress::LocalHost,directory.serverPort());require(wait([&]{return observer.is_registered() && observer.capabilities().contains(provider);}),"observer registry snapshot");
 AudioPipeline engine(test_fixture::synthetic_capture_factory());QString error;
 QJsonObject graph{{"version",2},{"graph_id","receiver"},{"nodes",QJsonArray{QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","scalar"},{"stream_id","chosen"},{"provider_uuid",provider.toString()}}}},QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","ScalarView"}}}}}},{"connections",QJsonArray{QJsonObject{{"outNodeId",0},{"outPortIndex",0},{"inNodeId",1},{"inPortIndex",0}}}}};
 require(engine.apply_graph(graph,error),qPrintable(error));engine.start_analyzer();
 QJsonObject cfg{{"device",QJsonObject{{"uuid",QUuid::createUuid().toString()}}},{"aggregator",QJsonObject{{"enabled",true},{"host","127.0.0.1"},{"port",directory.serverPort()}}}};
 NetworkRuntime runtime(engine,cfg);require(runtime.refresh(error),qPrintable(error));require(wait([&]{return server.subscriptions==1;}),"initial stream subscribed");
 auto available=[&]{const auto frames=engine.latest_port_frames({{"receiver",1,PortDirection::Input,0}});return frames.size()==1 && frames[0].frame.available;};
 TypedFrame frame;frame.type=FrameType::Scalar;frame.sample_rate=48000;frame.stream_id="chosen";frame.values={.5f};frame.sequence=1;frame.epoch=77;frame.producer_epoch=700;frame.generation=9;
 server.send(frame);require(wait(available),"initial DATA accepted");
 advertiser.set_capabilities(caps("88"));require(wait([&]{return server.subscriptions==2;}),"incarnation resubscribe");settle_packets();require(!available(),"reconnect stale local cache");
 frame.sequence=999;frame.generation=UINT64_MAX;frame.producer_epoch=UINT64_MAX;server.send(frame);settle_packets();
 const bool stale=available();std::cout<<"registry_incarnation=88 received_DATA_epoch=77 old_incarnation_available="<<stale<<std::endl;
 require(!stale,"old incarnation reappeared after registry replacement");
 frame.available=false;frame.sequence=0;frame.values.clear();server.send(frame);settle_packets();require(!available(),"old unavailable marker accepted");
 frame.available=true;frame.values={.75f};frame.epoch=99;frame.sequence=1000;server.send(frame);settle_packets();require(!available(),"unadvertised future incarnation accepted");
 uint64_t expected_transport=88;auto current=[&](uint64_t sequence){const auto values=engine.latest_port_frames({{"receiver",1,PortDirection::Input,0}});return values.size()==1 && values[0].frame.available && values[0].frame.epoch==expected_transport && values[0].frame.producer_epoch==700 && values[0].frame.generation==9 && values[0].frame.sequence==sequence;};
 frame.epoch=88;frame.sequence=2;frame.generation=9;frame.producer_epoch=700;server.send(frame);require(wait([&]{return current(2);}),"current incarnation rejected or metadata relabeled/poisoned");
 frame.epoch=77;frame.sequence=1001;server.send(frame);settle_packets();require(current(2),"late old packet replaced current cache");
 server.peer->abort();require(wait([&]{return server.subscriptions==3;}),"same binding TCP reconnect");settle_packets();require(!available(),"disconnect failed to clear cache");
 server.send(frame);settle_packets();require(!available(),"old incarnation accepted after retry reconnect");frame.epoch=88;frame.sequence=3;server.send(frame);require(wait([&]{return current(3);}),"current incarnation retry failed");
 auto remapped=caps("99");auto rows=remapped["streams"].toArray();auto row=rows[0].toObject();row["node_id"]=6;rows[0]=row;remapped["streams"]=rows;advertiser.set_capabilities(remapped);
 require(wait([&]{return server.subscriptions==4;}),"registry port identity remap did not reconnect");settle_packets();require(!available(),"remap retained stale cache");expected_transport=99;
 frame.epoch=88;frame.sequence=1002;server.send(frame);settle_packets();require(!available(),"remap accepted old incarnation");frame.epoch=99;frame.sequence=4;server.send(frame);require(wait([&]{return current(4);}),"valid remap delivery rejected");
 auto malformed=remapped;malformed["publisher_incarnation"]="099";advertiser.set_capabilities(malformed);settle_packets();require(server.subscriptions==4 && current(4),"malformed metadata partially replaced live binding/cache");
 frame.sequence=5;server.send(frame);require(wait([&]{return current(5);}),"valid delivery lost after malformed metadata");
 auto immutable=remapped;auto changed_rows=immutable["streams"].toArray();auto changed=changed_rows[0].toObject();changed["node_id"]=7;changed_rows[0]=changed;immutable["streams"]=changed_rows;advertiser.set_capabilities(immutable);
 require(wait([&]{return !advertiser.is_registered();}),"same incarnation binding mutation not refused");
 const auto retained=observer.capabilities().value(provider);require(retained["publisher_incarnation"].toString()=="99" && retained["streams"].toArray()[0].toObject()["node_id"].toInt()==6,"refused immutable binding replaced confirmed metadata");
 std::cout<<"late/retry/remap/current metadata/atomic refusal passed"<<std::endl;
 engine.stop_analyzer();
}

static QByteArray engine_fragment(uint8_t kind,uint64_t serial,uint32_t total,uint32_t offset,const QByteArray &chunk) {
    QByteArray bytes;auto put=[&](uint64_t value,int n){for(int i=0;i<n;++i)bytes.append(char(value>>(i*8)));};
    put(1,2);put(kind,1);put(serial,8);put(total,4);put(offset,4);put(chunk.size(),4);bytes.append(chunk);return bytes;
}
static void fragment_boundaries() {
    {
        DeliveryPeer server;require(server.listen(QHostAddress::LocalHost,0),"fragment valid listen");EngineClient client("127.0.0.1",server.serverPort());require(wait([&]{return client.snapshot().epoch==42;}),"fragment valid handshake");
        require(client.subscribe_port_frames({{"main",1,PortDirection::Input,0}}),"fragment valid subscribe");require(wait([&]{return client.port_delivery_mode()==EnginePort::PortDeliveryMode::CanonicalPortsV1;}),"fragment valid ACK");
        port_delivery::Frames batch;batch.snapshot=true;batch.graph_id="main";batch.nonce=server.nonce;batch.session_epoch=42;batch.sequence=1;batch.delivery={1,10};
        DeliveredPortFrame value;value.endpoint={"main",1,PortDirection::Input,0};value.kind=domain::NodeKind::MelView;value.delivery=batch.delivery;value.contract_known=true;value.contract.type=FrameType::Mel;value.contract.sample_rate=48000;value.contract.columns=17000;
        value.frame.type=FrameType::Mel;value.frame.sample_rate=48000;value.frame.sequence=1;value.frame.values.assign(17000,.5f);batch.entries={value};
        std::vector<uint8_t> storage(100000);const auto encoded=port_delivery::encode(batch,storage);require(bool(encoded) && encoded.written_bytes>65535,"fragment valid grammar");
        const QByteArray payload(reinterpret_cast<const char*>(storage.data()),encoded.written_bytes);
        for(qsizetype off=0;off<payload.size();off+=60000)require(server.peer->send_packet(10,engine_fragment(20,1,payload.size(),off,payload.mid(off,60000))),"fragment valid send");
        require(wait([&]{return client.snapshot().port_frames.size()==1 && client.snapshot().port_frames[0].frame.values.size()==17000;}),"valid consecutive fragments not assembled");
        server.revision=2;server.status(State::Running);require(wait([&]{return client.snapshot().graph_revision==2;}),"completed assembly blocked following status");client.disconnect_remote();
    }
    {
        AudioPipeline engine(test_fixture::synthetic_capture_factory());engine.start_analyzer();EngineServer server(engine);require(server.listen(QHostAddress::LocalHost,0,true),"server fragment listen");TcpBDSPSocket peer;
        peer.connectToHost(QHostAddress::LocalHost,server.port());require(wait([&]{return peer.state()==QAbstractSocket::ConnectedState;}),"server fragment connection");const auto revision=engine.graph_revision();
        require(peer.send_packet(10,engine_fragment(5,1,70000,0,QByteArray(60000,char(0)))),"server partial command");settle_packets();
        QByteArray stop;QDataStream out(&stop,QIODevice::WriteOnly);out.setVersion(QDataStream::Qt_6_0);out.setByteOrder(QDataStream::LittleEndian);out.setFloatingPointPrecision(QDataStream::SinglePrecision);
        out<<quint16(1)<<quint64(1)<<quint8(EngineCommand::StopAnalyzer)<<quint64(std::numeric_limits<uint64_t>::max())<<qint32(0)<<quint32(0)<<QString{}<<QString{}<<quint16(0)<<QByteArray{};
        require(peer.send_packet(5,stop),"server interleaved stop");require(wait([&]{return peer.state()==QAbstractSocket::UnconnectedState;}),"server did not abort interleaved command");
        require(engine.current_status("Analyzer").state==State::Running && engine.graph_revision()==revision,"server applied interleaved stop");engine.stop_analyzer();
    }
}
static void stopped_poison_and_interleave(bool interleave) {
 DeliveryPeer server;require(server.listen(QHostAddress::LocalHost,0),"listen");EngineClient client("127.0.0.1",server.serverPort());require(wait([&]{return client.snapshot().epoch==42;}),"handshake");
 require(client.subscribe_port_frames({{"main",1,PortDirection::Input,0}}),"subscribe");require(wait([&]{return client.port_delivery_mode()==EnginePort::PortDeliveryMode::CanonicalPortsV1;}),"ACK");
 int callbacks=0;client.set_port_frame_handler([&](DeliveredPortFrame){++callbacks;});
 TypedFrame data;data.type=FrameType::Mel;data.sample_rate=48000;data.sequence=10;data.generation=99;data.epoch=800;data.producer_epoch=801;data.graph_revision=900;data.values={.5f};
 server.canonical({data},10,1,true);require(wait([&]{return callbacks==1;}),"initial available");
 if(interleave){
  QByteArray b;auto put=[&](uint64_t value,int n){for(int i=0;i<n;++i)b.append(char(value>>(i*8)));};put(1,2);put(20,1);put(1,8);put(70000,4);put(0,4);put(60000,4);b.append(QByteArray(60000,char(0)));
  require(server.peer->send_packet(10,b),"fragment send");settle_packets();server.revision=2;server.status(State::Running);settle_packets();
  const bool accepted=client.snapshot().graph_revision==2;std::cout<<"active_partial_20_interleaved_status7_accepted="<<accepted<<std::endl;require(!accepted,"interleaved status changed snapshot");require(wait([&]{return client.snapshot().epoch==0;}),"interleaved message did not close poisoned connection");client.disconnect_remote();return;
 }
 server.status(State::Stopped);require(wait([&]{return client.snapshot().statuses.value("Analyzer").state==State::Stopped;}),"stopped status");
 server.sequence=998;server.canonical({data},999,1);settle_packets();require(callbacks==1,"Stopped available callback leaked");
 server.sequence=2;TypedFrame clear;clear.type=FrameType::Mel;clear.available=false;server.canonical({clear},11,1);settle_packets();
 const bool cleared=callbacks==2 && !client.snapshot().port_frames[0].frame.available;
 std::cout<<"stopped_available_rejected=1 valid_later_clear_delivered="<<cleared<<" callbacks="<<callbacks<<std::endl;
 server.status(State::Running);require(wait([&]{return client.snapshot().statuses.value("Analyzer").state==State::Running;}),"resume status");data.sequence=11;server.canonical({data},12,1);settle_packets();
 const bool resumed=callbacks==3;std::cout<<"valid_resumed_frame_delivered="<<resumed<<std::endl;client.disconnect_remote();require(cleared && resumed,"Stopped rejection poisoned accepted fences");
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if(app.arguments().contains("--stopped-poison-only")){stopped_poison_and_interleave(false);return 0;}
        if(app.arguments().contains("--fragment-interleave-only")){stopped_poison_and_interleave(true);fragment_boundaries();return 0;}
        if(app.arguments().contains("--registry-incarnation-only")){registry_incarnation();return 0;}
        if(app.arguments().contains("--complete-coverage-only")){complete_coverage();return 0;}
        if(app.arguments().contains("--stopped-delivery-only")){stopped_delivery_fences();server_stop_start_queue();return 0;}
        if(app.arguments().contains("--default-preparation-only")) {
            AudioPipeline engine(test_fixture::synthetic_capture_factory());
            const auto ports=engine.latest_port_frames();
            auto configured=[&](int node,PortDirection direction,int port,FrameType type){return std::any_of(ports.begin(),ports.end(),[&](const auto &value){return value.endpoint==EndpointIdentity{"main",node,direction,uint16_t(port)} && value.frame.type==type && !value.frame.available;});};
            require(configured(4,PortDirection::Output,0,FrameType::History) && configured(4,PortDirection::Output,1,FrameType::Scalar),"Default Spectrogram lost History/count ports");
            require(configured(8,PortDirection::Output,0,FrameType::WledPacket) && configured(13,PortDirection::Input,0,FrameType::WledPacket),"Default WLED encode/destination endpoints changed");
            require(std::none_of(ports.begin(),ports.end(),[](const auto &value){return value.endpoint.node==13 && value.endpoint.direction==PortDirection::Output;}),"Default destination has fake output");return 0;
        }
        if(app.arguments().contains("--canonical-ports-only")){canonical_protocol();return 0;}
        NetworkPublisher producer;
        producer.advertise("mel", FrameType::Mel);
        require(producer.listen(), "producer listen");
        QJsonObject graph{
            {"version", 2},
            {"nodes", QJsonArray{QJsonObject{{"id", 0},
                                             {"internal-data", QJsonObject{{"model-name", "NetworkInput"},
                                                                           {"data_type", "mel"},
                                                                           {"stream_id", "mel"},
                                                                           {"host", "127.0.0.1"},
                                                                           {"port", producer.port()}}}},
                                 QJsonObject{{"id", 1}, {"internal-data", QJsonObject{{"model-name", "MelView"}}}}}},
            {"connections",
             QJsonArray{QJsonObject{{"outNodeId", 0}, {"outPortIndex", 0}, {"inNodeId", 1}, {"inPortIndex", 0}}}}};
        TypedFrame frame;
        frame.type = FrameType::Mel;
        frame.sample_rate = 48000;
        frame.values.assign(50, 0.125f);
        frame.generation = 99;
        frame.sequence = 1;
        auto publish = [&] {
            frame.sequence++;
            producer.publish("mel", frame);
        };
        {
            EngineClient local(test_fixture::synthetic_capture_factory());
            EngineCommand command;
            command.kind = EngineCommand::ApplyGraph;
            command.graph = graph;
            local.submit(command);
            require(wait([&] { return local.snapshot().graph_revision > 0; }), "local apply");
            require(local.subscribe_port_frames({{"main",1,PortDirection::Input,0}}),"local canonical subscription");
            auto canonical_snapshot=local.snapshot().port_frames;
            require(canonical_snapshot.size()==1 && canonical_snapshot[0].endpoint.direction==PortDirection::Input,"local sink exposed fake output");
            command.kind = EngineCommand::StartAnalyzer;
            local.submit(command);
            require(wait([&] { return local.snapshot().statuses.value("Analyzer").state == State::Running; }),
                    "local start");
            bool received = false, cleared = false;
            local.set_view_handler([&](ViewFrame v) {
                if (v.views.contains(1)) {
                    received |= v.views[1].available && v.views[1].generation == 99;
                    cleared |= received && !v.views[1].available;
                }
            });
            require(wait([&] {
                        publish();
                        return received;
                    }),
                    "local origin generation incorrectly fenced against receiver generation");
            auto disconnected = graph;
            disconnected["connections"] = QJsonArray{};
            command.kind = EngineCommand::ApplyGraph;
            command.graph = disconnected;
            const auto previous_revision = local.snapshot().graph_revision;
            cleared = false;
            local.submit(command);
            require(wait([&] { return local.snapshot().graph_revision > previous_revision && cleared; }),
                    "local apply with last input disconnected lost the clear view");
            command.graph = graph;
            received = false;
            local.submit(command);
            require(wait([&] {
                        publish();
                        return received;
                    }),
                    "local reconnect after apply");
            cleared = false;
            command.kind = EngineCommand::StopAnalyzer;
            local.submit(command);
            require(wait([&] { return cleared; }), "local stopped view did not clear");
        }
        {
            AudioPipeline engine(test_fixture::synthetic_capture_factory());
            QString error;
            require(engine.apply_graph(graph, error), qPrintable(error));
            EngineServer server(engine);
            require(server.listen(QHostAddress::LocalHost, 0, true), "engine listen");
            engine.start_analyzer();
            NetworkRuntime network(engine);
            require(network.refresh(error), qPrintable(error));
            EngineClient remote("127.0.0.1", server.port());
            require(wait([&] { return remote.snapshot().epoch != 0; }), "remote handshake");
            bool canonical_received=false,canonical_cleared=false;
            remote.set_port_frame_handler([&](DeliveredPortFrame value){
                if(value.endpoint==EndpointIdentity{"main",1,PortDirection::Input,0}) {
                    canonical_received|=value.frame.available && value.frame.generation==99;
                    canonical_cleared|=canonical_received && !value.frame.available;
                }
            });
            require(remote.subscribe_port_frames({{"main",1,PortDirection::Input,0}},&error),qPrintable(error));
            require(wait([&]{return remote.port_delivery_mode()==EnginePort::PortDeliveryMode::CanonicalPortsV1;}),"remote canonical ACK");
            require(wait([&]{publish();return canonical_received;}),"remote canonical sink input delivery");
            require(remote.snapshot().port_frames.size()==1,"selected canonical snapshot incomplete or contains other endpoints");
            bool received = false, cleared = false,revived_after_stop=false,stop_check=false;
            remote.set_view_handler([&](ViewFrame v) {
                if (v.views.contains(1)) {
                    revived_after_stop|=stop_check && cleared && v.views[1].available;
                    received |= v.views[1].available && v.views[1].generation == 99;
                    cleared |= received && !v.views[1].available;
                }
            });
            require(wait([&] {
                        publish();
                        return received;
                    }),
                    "remote per-node origin generation incorrectly fenced");
            EngineCommand apply;
            apply.kind = EngineCommand::ApplyGraph;
            apply.graph = graph;
            apply.graph["connections"] = QJsonArray{};
            const auto previous_revision = remote.snapshot().graph_revision;
            cleared = false;
            remote.submit(apply);
            require(wait([&] { return remote.snapshot().graph_revision > previous_revision && cleared; }),
                    "remote apply ACK/revision ordering lost the disconnected view marker");
            apply.graph = graph;
            received = false;
            remote.submit(apply);
            require(wait([&] {
                        publish();
                        return received;
                    }),
                    "remote reconnect after apply");
            cleared = false;
            canonical_cleared=false;stop_check=true;
            engine.stop_analyzer();
            require(wait([&] { return cleared; }), "remote stopped view did not clear");
            require(wait([&]{return canonical_cleared;}),"remote stopped canonical destination did not clear");
            QElapsedTimer settle;settle.start();wait([&]{publish();return settle.elapsed()>=80;});
            require(!revived_after_stop,"Queued data resurrected stopped legacy view");
            const auto stopped=remote.snapshot().port_frames;
            require(!stopped.empty() && std::all_of(stopped.begin(),stopped.end(),[](const auto &value){return !value.frame.available;}),"Queued data resurrected stopped canonical cache");
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
