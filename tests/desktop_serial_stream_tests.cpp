#include "numeric_transport_fixture.h"
#include <desktop/qt_byte_transport.h>
#include <pipeline/serial_config.h>
#include <QJsonArray>
#include <cstdio>
using namespace transport_fixture;
namespace nd=bdsp_protocol::numeric_data;
class FakeUart final:public QIODevice{
public:
    QByteArray received,sent;qint64 pending=0;
    FakeUart(){open(ReadWrite);}bool isSequential()const override{return true;}
    qint64 bytesAvailable()const override{return received.size()+QIODevice::bytesAvailable();}
    qint64 bytesToWrite()const override{return pending;}
    void inject(QByteArray bytes){received+=bytes;emit readyRead();}
    void drain(){pending=0;emit bytesWritten(1);}
protected:
    qint64 readData(char* out,qint64 n)override{auto size=std::min(n,qint64(received.size()));std::copy_n(received.data(),size,out);received.remove(0,size);return size;}
    qint64 writeData(const char* data,qint64 n)override{auto size=std::min(n,qint64(3));sent.append(data,size);pending+=size;return size;}
};
static QByteArray packet(quint8 id,QByteArray payload){std::vector<std::uint8_t> bytes(payload.size()+64);auto result=bdsp_protocol::encode_packet(id,
    {reinterpret_cast<const std::uint8_t*>(payload.data()),std::size_t(payload.size())},bytes);require(bool(result),"UART BDSP envelope");return {reinterpret_cast<const char*>(bytes.data()),qsizetype(result.written_bytes)};}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    NumericEngine engine;Replies replies(engine);auto g=graph("uart",10000);replies.accepted(apply(g));
    FrameProbe observation(engine,sink(g));
    FakeUart uart;desktop::QtDeviceByteTransport io(uart,12);desktop::QtPacketChannel channel(uart,io);
    remote_numeric::CodecStorage encoder,decoder;auto arena=std::make_shared<remote_numeric::Arena>(encoder.limits.frame);remote_numeric::Binding binding;
    require(remote_numeric::make_binding(sink(g),g.nodes[1].inputs[0].contract,1,arena,binding)==Status::ok,"UART numeric binding");
    std::vector<std::uint64_t> samples(10000,9007199254740993ULL);PlaneLayout plane{0,{8,0}};
    auto local=contract(10000);NumericView input{&local,std::as_bytes(std::span(samples)),std::span(&plane,1),{10000,1}};
    NumericContract wireContract;NumericView wireView;require(encoder.project(input,wireContract,wireView)==Status::ok,"UART exact wire projection");
    nd::Message message;message.contract=wireContract;message.endpoint=binding.view().endpoint;message.envelope={1,2,1,3,1,4,0};
    message.stamp.sequence=1;message.stamp.producer_epoch=99;message.stamp.generation=1;message.stamp.stream_id="uart";
    message.numeric={wireView.storage,wireView.planes,wireView.actual};
    auto data=[&](std::uint64_t sequence){message.stamp.sequence=sequence;message.envelope.delivery_sequence=sequence;
        const auto encoded=nd::encode(message,encoder.wire,encoder.scratch,encoder.limits);require(bool(encoded),"DATA4 encode");
        std::span raw(reinterpret_cast<const std::uint8_t*>(encoder.wire.data()),encoded.produced);QByteArray result;std::array<std::uint8_t,511> fragment;
        for(std::size_t offset=0;offset<raw.size();offset+=503){const auto f=bdsp_protocol::encode_fragment(raw,offset,fragment,503);require(bool(f),"bounded UART fragment");
            result+=packet(nd::FragmentId,{reinterpret_cast<const char*>(fragment.data()),qsizetype(f.written_bytes)});}return result;};
    int delivered=0;bool stopInCallback=false;
    QObject::connect(&channel,&desktop::QtPacketChannel::packetReceived,&app,[&](quint8 id,QByteArray payload){
        require(id==nd::FragmentId,"canonical fragment ID");std::span<const std::uint8_t> complete;
        const auto status=decoder.assembler.accept({reinterpret_cast<const std::uint8_t*>(payload.data()),std::size_t(payload.size())},complete);
        if(status==Status::would_block)return;require(status==Status::ok,"UART reassembly");nd::Message decoded;
        require(decoder.decode(complete,decoded)==Status::ok&&decoder.restore(decoded,binding)==Status::ok,"UART DATA4 decode/native restore");
        require(decoded.envelope.delivery_generation==decoded.stamp.generation&&!decoded.envelope.request,"UART event delivery fence");
        require(engine.feed_numeric(0,decoded.frame_view())==Status::ok,"UART canonical graph execution");++delivered;
        if(stopInCallback){channel.reset();decoder.assembler.reset();replies.accepted(apply(g));}
    });
    const auto first=data(1);for(char byte:first)uart.inject(QByteArray(1,byte));
    require(delivered==1&&wait([&]{return observation.available()&&value(observation.latest(),9999)==9007199254740993ULL;}),"fragmented UART exact64 actual graph");
    // Shared framing writer retains suffixes while QIODevice accepts 3 bytes.
    const auto outbound=QByteArray("control-frame");require(channel.send_packet(28,outbound)==Status::ok,"UART write queue");
    while(channel.pending_bytes())uart.drain();require(uart.sent==packet(28,outbound),"partial UART write changed bytes");
    stopInCallback=true;uart.inject(data(2)+data(3));require(delivered==2,"Stop/reset admitted coalesced stale packet");
    stopInCallback=false;message.stamp.producer_epoch=100;
    uart.inject(data(1));require(delivered==3&&wait([&]{auto f=observation.latest();return observation.equals(9007199254740993ULL)&&f.frame.view().stamp.producer_epoch==100;}),"UART new incarnation/rebind");
    QJsonObject config{{"unknown",42},{"network",QJsonObject{{"serial",QJsonObject{{"port_name","explicit-fixture"},{"baud_rate",115200},{"input_node",0},{"unknown-serial","preserved"}}}}}};
    require(bool(read_serial_binding(config)),"explicit UART config");QCommandLineParser options;add_serial_options(options);
    require(options.parse({"fixture","--serial-baud","230400"}),"serial CLI parse");QString error;
    require(apply_serial_options(options,config,error)&&config["unknown"]==42&&config["network"].toObject()["serial"].toObject()["unknown-serial"]=="preserved","serial override lost unknown config");
    auto network=config["network"].toObject(),serial=network["serial"].toObject();serial["baud_rate"]="115200";network["serial"]=serial;config["network"]=network;
    require(!read_serial_binding(config),"string baud rate accepted");
    std::puts("fake UART DATA4 exact64 + actual engine + partial writes + callback Stop/reset + new incarnation passed");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
