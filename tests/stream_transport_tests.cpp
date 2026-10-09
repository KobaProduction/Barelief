#include "numeric_transport_fixture.h"
#include <control/numeric_desktop_host.h>
#include <wled_protocol/wled_packet.h>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <cstdio>
using namespace transport_fixture;
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QUdpSocket receiver;require(receiver.bind(QHostAddress::LocalHost,quint16(0)),"UDP receiver");
    StructuredContract contract{"wled-audio-sync",1,"audio-sync-44-le",{44,44}};domain::OwnedProcessingContract owner;
    require(domain::own_processing_contract(contract,bdsp_protocol::numeric_control::desktop_limits().frame,4096,owner)==Status::ok,"structured contract");
    domain::ConfiguredProcessingNode source;source.id=0;source.kind="NetworkInput";source.outputs={{0,domain::PortDirection::Output,owner}};
    domain::ConfiguredProcessingNode output;output.id=1;output.kind="WLEDOutput";output.inputs={{0,domain::PortDirection::Input,owner}};
    domain::EndpointProcessingParameters parameters;
    parameters.values={{"destination",std::string("127.0.0.1")},{"interface",std::string("127.0.0.1")},{"port",NumericLiteral{std::uint64_t(receiver.localPort())}},{"cadence_ms",NumericLiteral{std::uint64_t(1)}}};output.parameters=parameters;
    domain::NumericProcessingGraph graph;graph.graph_id="udp";graph.nodes={source,output};graph.edges={{0,1,0,0}};
    std::unique_ptr<NumericEngine> sameThread;std::unique_ptr<NumericDesktopHost> worker;
    if(app.arguments().contains("--same-thread"))sameThread=std::make_unique<NumericEngine>();else worker=std::make_unique<NumericDesktopHost>();
    EnginePort& port=worker?static_cast<EnginePort&>(*worker):static_cast<EnginePort&>(*sameThread);
    auto feedFrame=[&](const DataFrameView& frame){return worker?worker->feed_numeric(0,frame):sameThread->feed_numeric(0,frame);};
    Replies replies(port);replies.accepted(apply(graph));
    std::uint8_t next=0;std::size_t packets=0;
    // Apply/Start may legitimately send initial silence while their reply wait
    // pumps events. Remove it before isolating the blocked-GUI input delivery.
    while(receiver.hasPendingDatagrams()){
        const auto bytes=receiver.receiveDatagram().data();wled_protocol::DecodedPacket decoded;
        require(wled_protocol::decode({reinterpret_cast<const std::uint8_t*>(bytes.data()),std::size_t(bytes.size())},decoded,{0,44}).status==wled_protocol::Status::Ok&&decoded.counter==next++,"startup UDP wire/counter");++packets;
    }
    std::array<float,50> mel;mel.fill(30);wled_protocol::WledPacket packet{};
    require(wled_protocol::encode_mel(mel,100,2,1,23,packet,{50,44}).status==wled_protocol::Status::Ok,"WLED fixture");
    FrameStamp stamp;stamp.sequence=1;stamp.producer_epoch=101;stamp.generation=1;
    require(feedFrame({stamp,StructuredView{&contract,std::as_bytes(std::span(packet))}})==Status::ok,"actual structured ingress");
    // No GUI event pumping: the actual socket sender must keep running while
    // the GUI thread is blocked, not merely catch up when events resume.
    QThread::msleep(60);
    bool nonzero=false,silent=false;QElapsedTimer blocked;blocked.start();
    while(!nonzero&&blocked.elapsed()<200){
        if(!receiver.hasPendingDatagrams()&&!receiver.waitForReadyRead(int(200-blocked.elapsed())))break;
        while(receiver.hasPendingDatagrams()){
            const auto bytes=receiver.receiveDatagram().data();wled_protocol::DecodedPacket decoded;
            require(wled_protocol::decode({reinterpret_cast<const std::uint8_t*>(bytes.data()),std::size_t(bytes.size())},decoded,{0,44}).status==wled_protocol::Status::Ok&&decoded.counter==next++,"blocked GUI UDP wire/counter");
            ++packets;nonzero|=decoded.magnitude==2;
        }
    }
    require(nonzero,"WLED UDP stops while GUI event loop is blocked");
    auto receive=[&]{while(receiver.hasPendingDatagrams()){
        const auto bytes=receiver.receiveDatagram().data();require(bytes.size()==44,"UDP packet size");
        wled_protocol::DecodedPacket decoded;require(wled_protocol::decode({reinterpret_cast<const std::uint8_t*>(bytes.data()),std::size_t(bytes.size())},decoded,{0,44}).status==wled_protocol::Status::Ok,"canonical UDP wire");
        require(decoded.counter==next++,"destination counter including wrap");++packets;nonzero|=decoded.magnitude==2;silent|=decoded.magnitude==0;
    }return packets>=260&&nonzero&&silent;};
    const auto cadence_start=packets;QElapsedTimer cadence_timer;cadence_timer.start();
    const auto cadence_ok=wait(receive,6000);
    if(!cadence_ok)std::fprintf(stderr,"UDP cadence diagnostics: packets=%zu fresh=%zu elapsed=%lld nonzero=%d silent=%d next=%u warning=%s\n",
        packets,packets-cadence_start,static_cast<long long>(cadence_timer.elapsed()),int(nonzero),int(silent),unsigned(next),
        qPrintable(worker?worker->network_warning():sameThread->network_warning()));
    require(cadence_ok,"actual UDP cadence/stale/counter wrap");require(packet[17]==23,"producer packet mutated");
    const auto stopped=packets;
    require(wait([&]{receive();return packets>stopped;}),"continuous UDP output");
    stamp.sequence=2;stamp.available=false;require(feedFrame({stamp,StructuredView{&contract,{}}})==Status::ok,"typed unavailable ingress");
    require(wait([&]{receive();return packets>stopped+2;}),"unavailable silence continues");
std::puts("canonical engine WLED UDP cadence/counter/wrap/stale/Stop/Start passed");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
