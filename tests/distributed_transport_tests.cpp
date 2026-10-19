#include <bdsp_protocol/numeric_data_message.h>
#include "numeric_transport_fixture.h"
#include <control/numeric_routing_adapter.h>
#include <control/numeric_desktop_host.h>
#include <aggregator/aggregator_client.h>
#include <aggregator/aggregator_server.h>
#include <QJsonArray>
#include <cstdio>
using namespace transport_fixture;
static QJsonObject advertised(NumericEngine& e,quint16 port,const char* graphId){
    return {{"control_version",4},{"data_version",int(bdsp_protocol::numeric_data::PayloadVersion)},{"stream_port",int(port)},
        {"publisher_incarnation",QString::number(e.snapshot().epoch)},{"graph_revision",QString::number(e.snapshot().graph_revision)},
        {"streams",QJsonArray{QJsonObject{{"id","relay"},{"graph_id",graphId},{"node_id",1},{"direction","input"},{"port_index",0},{"payload_version",int(bdsp_protocol::numeric_data::PayloadVersion)}}}}};
}
static QJsonObject binding(QUuid provider){return {{"network",QJsonObject{{"routes",QJsonArray{QJsonObject{{"node_id",0},{"provider_uuid",provider.toString(QUuid::WithoutBraces)},{"stream_id","relay"},
    // An offline UUID must never fall through to this otherwise valid direct route.
    {"host","127.0.0.1"},{"port",1},{"remote_endpoint",QJsonObject{{"graph_id","wrong"},{"node_id",1},{"direction","input"},{"port_index",0}}}}}}}}};}
class WorkerDirectory {
    QThread thread;QObject* worker=new QObject;std::unique_ptr<AggregatorServer> server;
public:
    quint16 port=0;
    WorkerDirectory(){worker->moveToThread(&thread);QObject::connect(&thread,&QThread::finished,worker,&QObject::deleteLater);thread.start();
        QMetaObject::invokeMethod(worker,[this]{server=std::make_unique<AggregatorServer>();if(server->start(0))port=server->serverPort();},Qt::BlockingQueuedConnection);
        require(port!=0,"worker directory listen");}
    ~WorkerDirectory(){QMetaObject::invokeMethod(worker,[this]{server.reset();},Qt::BlockingQueuedConnection);thread.quit();thread.wait();}
    void stop(){QMetaObject::invokeMethod(worker,[this]{server->stop();},Qt::BlockingQueuedConnection);}
};
static void worker_routing(){
    WorkerDirectory directory;NumericDesktopHost a,b,c;Replies ar(a),br(b),cr(c);
    auto ga=graph("worker-a"),gb=graph("worker-b"),gc=graph("worker-c");
    ar.accepted(apply(ga));br.accepted(apply(gb));cr.accepted(apply(gc));

    require(a.listen(QHostAddress::LocalHost,0,true)&&b.listen(QHostAddress::LocalHost,0,true)&&c.listen(QHostAddress::LocalHost,0,true),"worker CONTROL2 listeners");
    const auto aid=QUuid::createUuid(),bid=QUuid::createUuid();a.set_registry_identity(aid);b.set_registry_identity(bid);
    a.set_registry_capabilities(advertised(*a.local_engine(),a.server_port(),"worker-a"));b.set_registry_capabilities(advertised(*b.local_engine(),b.server_port(),"worker-b"));
    for(auto* host:{&a,&b,&c})host->configure_registry(QHostAddress::LocalHost,directory.port);
    require(b.configure_network(binding(aid))&&c.configure_network(binding(bid)),"worker provider routes");
    FrameProbe workerObservation(*c.local_engine(),sink(gc));
    std::uint64_t sequence=0;auto arrives=[&](std::uint64_t expected){feed(a,1,++sequence,expected);return workerObservation.equals(expected);};
    require(wait([&]{return arrives(31);},6000),"worker registry discovery and two-hop DATA4");
    const auto baseline=workerObservation.latest().frame.view().stamp;
    require(c.subscribe_numeric_frames({sink(gc)}),"GUI-facing subscriber");
    // No processEvents or GUI callbacks here. Worker registry, control, stream
    // transports and route input must advance while the GUI thread is blocked.
    for(std::size_t i=0;i<160;++i){feed(a,1,++sequence,9007199254740993ULL+i);QThread::msleep(2);}
    QThread::msleep(250);
    require(workerObservation.equals(9007199254741152ULL),"TCP DATA4/registry routing stalls or loses pool slots with blocked GUI");
    const auto final=workerObservation.latest().frame.view().stamp;
    require(final.producer_epoch==baseline.producer_epoch&&final.generation==baseline.generation,"blocked GUI burst changed producer provenance");
    const auto before=c.snapshot().graph_revision;
    // CONTROL mutation travels through a remote worker host while GUI delivery
    // of the owning reply is intentionally paused.
    NumericDesktopHost remote({},"127.0.0.1",c.server_port());Replies rr(remote);remote.connect_remote();
    require(wait([&]{return remote.snapshot().graph_revision==before&&remote.snapshot().epoch!=0;}),"worker remote CONTROL2 bootstrap");
    auto changed=gc;auto extra=gc.nodes[0];extra.id=2;extra.enabled=false;changed.nodes.push_back(extra);
    const auto id=remote.submit(apply(changed));QThread::msleep(150);
    require(c.snapshot().graph_revision==before+1,"CONTROL2 mutation stalls with blocked GUI");
    require(wait([&]{return rr.values.contains(id)&&rr.values.at(id).accepted;}),"queued worker CONTROL reply lost");
    directory.stop();QThread::msleep(150);
    require(b.routing_diagnostics()["peer_count"].toInt()==0&&c.routing_diagnostics()["peer_count"].toInt()==0,"directory retirement stalls with blocked GUI");
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    AggregatorServer registry;require(registry.start(0),"directory listen");
    auto a=std::make_unique<NumericEngine>();NumericEngine b,c;auto ga=graph("a"),gb=graph("b"),gc=graph("c");
    // B has a second, unrelated ingress that must survive A's retirement.
    auto independent=gb.nodes;independent[0].id=2;independent[1].id=3;
    gb.nodes.insert(gb.nodes.end(),independent.begin(),independent.end());gb.edges.push_back({2,3,0,0});
    auto ar=std::make_unique<Replies>(*a);Replies br(b),cr(c);ar->accepted(apply(ga));br.accepted(apply(gb));cr.accepted(apply(gc));

    auto as=std::make_unique<NumericControlServer>(*a);NumericControlServer bs(b),cs(c);
    require(as->listen(QHostAddress::LocalHost,0,true)&&bs.listen(QHostAddress::LocalHost,0,true)&&cs.listen(QHostAddress::LocalHost,0,true),"three CONTROL2 servers");
    const auto aid=QUuid::createUuid(),bid=QUuid::createUuid();
    auto ac=std::make_unique<AggregatorClient>(AggregatorClientConfig{aid,ComponentType::core,as->port(),321});
    AggregatorClient bc({bid,ComponentType::core,bs.port(),322}),directoryB({QUuid::createUuid(),ComponentType::data_sink,0}),directoryC({QUuid::createUuid(),ComponentType::data_sink,0});
    ac->set_capabilities(advertised(*a,as->port(),"a"));bc.set_capabilities(advertised(b,bs.port(),"b"));
    for(auto* client:{ac.get(),&bc,&directoryB,&directoryC})client->connect_to_aggregator_server(QHostAddress::LocalHost,registry.serverPort(),1000);
    require(wait([&]{return !directoryB.resolve_engine(aid).isEmpty()&&!directoryC.resolve_engine(bid).isEmpty();}),"directory stream discovery");
    NumericRoutingAdapter rb(b,&directoryB),rc(c,&directoryC);require(rb.configure(binding(aid))&&rc.configure(binding(bid)),"provider routes");
    FrameProbe bObservation(b,sink(gb)),cObservation(c,sink(gc)),unrelated(b,{"b",3,domain::PortDirection::Input,0});
    NumericRemoteEndpoint fanout("127.0.0.1",cs.port());connect(fanout);fanout.subscribe_numeric_frames({sink(gc)});std::uint64_t observed=0;
    fanout.set_numeric_frame_handler([&](auto f){if(f.frame&&f.frame.view().stamp.available)observed=value(f);});
    std::uint64_t sequence=0;auto endToEnd=[&](std::uint64_t expected){feed(*a,1,++sequence,expected);return cObservation.equals(expected)&&observed==expected;};
    const auto relayed=wait([&]{return endToEnd(9007199254740993ULL);},6000);
    if(!relayed){
        std::fprintf(stderr,"routing B=%s C=%s fanout epoch=%llu graphrev=%llu observed=%llu warning=%s\n",
            QJsonDocument(rb.diagnostics()).toJson(QJsonDocument::Compact).constData(),QJsonDocument(rc.diagnostics()).toJson(QJsonDocument::Compact).constData(),
            (unsigned long long)fanout.snapshot().epoch,(unsigned long long)fanout.snapshot().graph_revision,(unsigned long long)observed,qPrintable(fanout.network_warning()));
        for(auto* probe:{&bObservation,&cObservation}){auto f=probe->latest();
            std::fprintf(stderr,"probe status=%d frame=%d available=%d producer=%llu sequence=%llu\n",int(f.status),bool(f.frame),f.frame&&f.frame.view().stamp.available,
                f.frame?(unsigned long long)f.frame.view().stamp.producer_epoch:0,f.frame?(unsigned long long)f.frame.view().stamp.sequence:0);}
    }
    require(relayed,"three-engine directory relay and fanout exact64");
    const auto firstProducer=cObservation.latest().frame.view().stamp.producer_epoch;
    ac->disconnect_from_aggregator_server();as.reset();
    require(wait([&]{return directoryB.resolve_engine(aid).isEmpty()&&!cObservation.available();},6000),"offline provider propagated typed unavailable");
    feed(b,1,1,73,2);require(wait([&]{return unrelated.equals(73);}),"retirement cleared unrelated branch");
    // Replace publisher process identity while retaining its persistent directory UUID.
    ac.reset();ar.reset();a.reset();a=std::make_unique<NumericEngine>();ar=std::make_unique<Replies>(*a);
    ar->accepted(apply(ga));as=std::make_unique<NumericControlServer>(*a);
    require(as->listen(QHostAddress::LocalHost,0,true),"replacement publisher server");
    ac=std::make_unique<AggregatorClient>(AggregatorClientConfig{aid,ComponentType::core,as->port(),321});
    ac->set_capabilities(advertised(*a,as->port(),"a"));ac->connect_to_aggregator_server(QHostAddress::LocalHost,registry.serverPort(),1000);sequence=0;
    require(wait([&]{return endToEnd(UINT64_MAX);},6000),"same UUID new publisher incarnation with restarted sequence");
    require(cObservation.latest().frame.view().stamp.producer_epoch!=firstProducer,"publisher provenance was rewritten or stale");
    require(unrelated.equals(73),"publisher restart reset unrelated branch");
    registry.stop();require(wait([&]{return !directoryB.is_registered()&&!directoryC.is_registered();}),"directory disconnection");
    require(wait([&]{return rb.diagnostics()["peer_count"].toInt()==0&&rc.diagnostics()["peer_count"].toInt()==0;}),"directory loss retained cached/direct fallback peer");
    worker_routing();std::puts("three canonical engines + directory + fanout + selective retirement + same UUID incarnation + blocked GUI worker DATA/CONTROL/directory passed");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
