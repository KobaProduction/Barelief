#include <pipeline/stream_transport.h>
#include <pipeline/audio_pipeline.h>
#include <pipeline/network_runtime.h>
#include <pipeline/serial_config.h>
#include <bdsp_protocol/stream_fragments.h>
#include <QCoreApplication>
#include <QJsonArray>
#include <array>
#include <iostream>
#include <stdexcept>
using namespace pipeline;
using signal_contracts::Status;
static void require(bool ok,const char *message) {if(!ok) throw std::runtime_error(message);}
class FakeUart final : public QIODevice {
public:
    QByteArray received,sent; qint64 pending=0;
    FakeUart(){open(ReadWrite);}
    bool isSequential() const override{return true;}
    qint64 bytesAvailable() const override{return received.size()+QIODevice::bytesAvailable();}
    qint64 bytesToWrite() const override{return pending;}
    void inject(const QByteArray &bytes){received+=bytes;emit readyRead();}
    void drain(){pending=0;emit bytesWritten(1);}
protected:
    qint64 readData(char *out,qint64 n) override {
        const auto count=std::min(n,qint64(received.size()));std::copy_n(received.data(),count,out);
        received.remove(0,count);return count;
    }
    qint64 writeData(const char *data,qint64 n) override {
        const auto count=std::min(n,qint64(3));sent.append(data,count);pending+=count;return count;
    }
};
static QByteArray packet(quint8 id,const QByteArray &payload) {
    std::array<uint8_t,65539> bytes{};
    auto encoded=bdsp_protocol::encode_packet(id,{reinterpret_cast<const uint8_t *>(payload.data()),size_t(payload.size())},bytes);
    require(bool(encoded),"BDSP packet fixture");return {reinterpret_cast<const char *>(bytes.data()),qsizetype(encoded.written_bytes)};
}
static QByteArray data(const TypedFrame &frame) {
    const auto encoded=stream::encode(frame);QByteArray result;
    std::array<uint8_t,bdsp_protocol::StreamFragmentBytes+8> fragment{};
    for(size_t offset=0;offset<encoded.size();offset+=bdsp_protocol::StreamFragmentBytes) {
        const auto chunk=bdsp_protocol::encode_fragment(encoded,offset,fragment);require(bool(chunk),"fragment fixture");
        result+=packet(13,{reinterpret_cast<const char *>(fragment.data()),qsizetype(chunk.written_bytes)});
    }
    return result;
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        AudioPipeline engine;QString error;
        QJsonObject graph{{"version",2},{"nodes",QJsonArray{
            QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","mel"},{"stream_id","uart"}}}},
            QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","Spectrogram"},{"history_size",4}}}},
            QJsonObject{{"id",2},{"internal-data",QJsonObject{{"model-name","SpectrogramView"}}}}}},
            {"connections",QJsonArray{
                QJsonObject{{"outNodeId",0},{"outPortIndex",0},{"inNodeId",1},{"inPortIndex",0}},
                QJsonObject{{"outNodeId",1},{"outPortIndex",0},{"inNodeId",2},{"inPortIndex",0}}}}};
        require(engine.apply_graph(graph,error),qPrintable(error));engine.start_analyzer();
        FakeUart uart;desktop::QtDeviceByteTransport io(uart,12);desktop::QtPacketChannel channel(uart,io);
        NetworkSubscriber subscriber;int delivered=0;bool ready=false;
        subscriber.set_status_handler([&](QString status){ready=status=="Running";if(!ready)engine.clear_ingress(0);});
        subscriber.set_frame_handler([&](TypedFrame frame){++delivered;engine.feed_typed(0,std::move(frame),engine.epoch(),engine.ingress_serial(0));});
        subscriber.start_channel(channel,"uart",FrameType::Mel);
        while(channel.pending_bytes())uart.drain();
        const auto subscribe=QByteArray::fromHex("000205")+"uart";
        require(uart.sent==packet(11,subscribe),"UART subscribe changed existing TCP wire");
        const auto ack=packet(12,subscribe);
        for(char byte:ack)uart.inject(QByteArray(1,byte));
        require(ready,"fragmented UART Ack");
        TypedFrame mel;mel.type=FrameType::Mel;mel.stream_id="uart";mel.sample_rate=48000;mel.sequence=1;mel.epoch=99;
        mel.values={1,2,3};const auto wire=data(mel);
        for(char byte:wire)uart.inject(QByteArray(1,byte));
        require(delivered==1 && engine.latest_views()[2].history==std::vector<std::vector<float>>{{1,2,3}},
                "UART packet did not execute actual desktop graph");
        TypedFrame unavailable;unavailable.type=FrameType::Mel;unavailable.available=false;unavailable.epoch=99;
        unavailable.stream_id="uart";
        uart.inject(data(unavailable));require(delivered==2 && !engine.latest_views()[2].available,"UART unavailable retained history");
        mel.sequence=2;uart.inject(data(mel));require(engine.latest_views()[2].history.size()==1,"UART resume failed");
        // Stop from a callback must fence remaining coalesced bytes and assembly.
        subscriber.set_frame_handler([&](TypedFrame frame){++delivered;engine.feed_typed(0,std::move(frame));subscriber.stop();});
        mel.sequence=3;const auto one=data(mel);mel.sequence=4;
        uart.inject(one+data(mel));require(delivered==4 && !engine.latest_views()[2].available,"Stop admitted coalesced stale packet");
        uart.sent.clear();subscriber.start_channel(channel,"uart",FrameType::Mel);
        while(channel.pending_bytes())uart.drain();uart.inject(ack);
        subscriber.set_frame_handler([&](TypedFrame frame){++delivered;engine.feed_typed(0,std::move(frame));});
        mel.sequence=1;mel.epoch=100;uart.inject(data(mel));
        require(delivered==5 && engine.latest_views()[2].history.size()==1,"Fresh UART binding retained old assembly/history");
        subscriber.stop();engine.stop_analyzer();
        QJsonObject config{{"unknown",42},{"network",QJsonObject{{"unknown-network",true},{"serial",QJsonObject{
            {"port_name","explicit-fixture"},{"baud_rate",115200},{"input_node",0},{"unknown-serial","preserved"}}}}}};
        const auto binding=read_serial_binding(config);
        require(binding && binding.configured && binding.input_node==0 && binding.endpoint.port_name=="explicit-fixture","Serial config mapping");
        QCommandLineParser options;add_serial_options(options);
        require(options.parse({"fixture","--serial-baud","230400"}),"Serial options parse");
        require(apply_serial_options(options,config,error) && config["unknown"]==42 &&
                config["network"].toObject()["serial"].toObject()["unknown-serial"]=="preserved","Serial override lost JSON extensions");
        auto network=config["network"].toObject();auto serial=network["serial"].toObject();serial["baud_rate"]="115200";
        network["serial"]=serial;config["network"]=network;
        require(!read_serial_binding(config),"String baud rate accepted");
        serial["baud_rate"]=115200;serial["input_node"]=1;network["serial"]=serial;config["network"]=network;
        NetworkRuntime runtime(engine,config);require(!runtime.refresh(error) && error.contains("NetworkInput"),"UART attached to DSP node");
        std::cout<<"Explicit UART typed-stream framing, desktop execution, Stop/rebind and config boundaries passed\n";
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
