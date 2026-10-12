#include <aggregator/aggregator_client.h>
#include <aggregator/aggregator_server.h>
#include <control/numeric_control_server.h>
#include <control/numeric_remote_endpoint.h>
#include <pipeline/numeric_engine.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonArray>
#include <cstdio>
#include <stdexcept>
static void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
static bool wait(std::function<bool()> f){QElapsedTimer t;t.start();while(!f()&&t.elapsed()<2500){QCoreApplication::processEvents();QThread::msleep(1);}return f();}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
AggregatorServer directory;require(directory.start(0),"directory listen");pipeline::NumericEngine engine;auto server=std::make_unique<pipeline::NumericControlServer>(engine);require(server->listen(QHostAddress::LocalHost,0,true),"numeric endpoint listen");
const auto id=QUuid::createUuid();AggregatorClient core({id,ComponentType::core,server->port(),321}),observer({QUuid::createUuid(),ComponentType::data_sink,0});
core.set_capabilities({{"control_version",4},{"data_version",5},{"stream_port",server->port()},{"publisher_incarnation",QString::number(engine.snapshot().epoch)},{"graph_revision","0"},{"streams",QJsonArray{}}});
core.connect_to_aggregator_server(QHostAddress::LocalHost,directory.serverPort(),1000);observer.connect_to_aggregator_server(QHostAddress::LocalHost,directory.serverPort(),1000);QString error;
require(wait([&]{return !observer.resolve_engine(id,&error).isEmpty();}),"canonical endpoint discovery");auto found=observer.resolve_engine(id,&error);require(found["port"].toInt()==server->port(),"endpoint port identity");pipeline::NumericRemoteEndpoint remote(found["address"].toString(),quint16(found["port"].toInt()));remote.connect_remote();require(wait([&]{return remote.snapshot().epoch!=0;}),"discovered numeric CONTROL handshake");
core.disconnect_from_aggregator_server();server.reset();require(wait([&]{return observer.resolve_engine(id,&error).isEmpty();}),"offline directory endpoint remains available");require(wait([&]{return remote.snapshot().epoch==0;}),"disconnected remote retains active epoch");
directory.stop();require(wait([&]{return !observer.is_registered();}),"directory loss ignored");require(observer.resolve_engine(id,&error).isEmpty(),"cached endpoint accepted after directory loss");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
