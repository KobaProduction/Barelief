#include "pipeline/stream_codec.h"
#include "pipeline/stream_transport.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTcpServer>
#include "core/network/tcp_packet_socket.h"
#include <cassert>
#include <iostream>
#include <bdsp_protocol/stream_fragments.h>
#include <array>
using namespace pipeline;
static bool wait(const std::function<bool()> &predicate, int ms = 2000) {
    QElapsedTimer t;
    t.start();
    while (!predicate() && t.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}
class ScriptedStreamPeer final : public QTcpServer {
public:
    TcpBDSPSocket *peer=nullptr;
    int subscriptions=0;
protected:
    void incomingConnection(qintptr descriptor) override {
        peer=new TcpBDSPSocket(this);assert(peer->setSocketDescriptor(descriptor));
        QObject::connect(peer,&TcpBDSPSocket::packetReceived,this,[this](quint8 id,QByteArray bytes) {
            if(id==11){++subscriptions;assert(peer->send_packet(12,bytes));}
        });
    }
};
static void send_frame(ScriptedStreamPeer &server,const TypedFrame &frame) {
    const auto bytes=stream::encode(frame);std::array<uint8_t,60008> fragment{};
    const auto encoded=bdsp_protocol::encode_fragment(bytes,0,fragment);assert(encoded);
    assert(server.peer->send_packet(13,QByteArray(reinterpret_cast<const char *>(fragment.data()),encoded.written_bytes)));
}
static void settle() {QElapsedTimer timer;timer.start();wait([&]{return timer.elapsed()>=60;},100);}
static void subscriber_fences(bool sequence_only=false) {
    TypedFrame frame;frame.type=FrameType::Mel;frame.sample_rate=48000;frame.epoch=90;frame.producer_epoch=100;
    frame.generation=1;frame.sequence=11;frame.values={1,2,3};frame.stream_id="chosen";
    if(!sequence_only) {
        ScriptedStreamPeer server;assert(server.listen(QHostAddress::LocalHost,0));
        NetworkSubscriber subscriber;int deliveries=0;bool recovering=false;
        subscriber.set_frame_handler([&](TypedFrame){++deliveries;});
        subscriber.set_status_handler([&](QString status){recovering|=status=="Recovering";});
        subscriber.start("127.0.0.1",server.serverPort(),"chosen",FrameType::Mel);
        assert(wait([&]{return server.subscriptions==1;}));settle();
        auto wrong=frame;wrong.stream_id="wrong";send_frame(server,wrong);settle();
        assert(deliveries==0 && recovering);subscriber.stop();
    }
    {
        ScriptedStreamPeer server;assert(server.listen(QHostAddress::LocalHost,0));
        NetworkSubscriber subscriber;int deliveries=0;TypedFrame delivered;
        subscriber.set_frame_handler([&](TypedFrame value){++deliveries;delivered=std::move(value);});
        subscriber.start("127.0.0.1",server.serverPort(),"chosen",FrameType::Mel);
        assert(wait([&]{return server.subscriptions==1;}));settle();
        send_frame(server,frame);assert(wait([&]{return deliveries==1;}));
        auto marker=frame;marker.available=false;marker.sequence=0;marker.values.clear();send_frame(server,marker);
        assert(wait([&]{return deliveries==2;}));assert(!delivered.available);
        frame.sequence=10;send_frame(server,frame);settle();assert(deliveries==2);
        frame.sequence=11;frame.discontinuity=true;send_frame(server,frame);settle();assert(deliveries==2);
        frame.sequence=12;frame.discontinuity=false;send_frame(server,frame);
        assert(wait([&]{return deliveries==3;}));assert(delivered.discontinuity);
        frame.sequence=13;send_frame(server,frame);assert(wait([&]{return deliveries==4;}));assert(!delivered.discontinuity);
        marker.generation=2;send_frame(server,marker);assert(wait([&]{return deliveries==5;}));
        frame.sequence=14;send_frame(server,frame);settle();assert(deliveries==5);
        frame.generation=2;frame.sequence=1;send_frame(server,frame);assert(wait([&]{return deliveries==6;}));
        frame.producer_epoch=101;frame.generation=1;frame.sequence=1;frame.values={4,5,6,7};send_frame(server,frame);
        assert(wait([&]{return deliveries==7;}));assert(delivered.values==frame.values && delivered.discontinuity);
        auto retired=frame;retired.producer_epoch=100;retired.generation=2;retired.sequence=2;retired.discontinuity=true;
        send_frame(server,retired);settle();assert(deliveries==7);
        frame.sequence=2;send_frame(server,frame);assert(wait([&]{return deliveries==8;}));assert(!delivered.discontinuity);
        retired.available=false;retired.sequence=0;retired.values.clear();send_frame(server,retired);settle();assert(deliveries==8);
        frame.sequence=3;send_frame(server,frame);assert(wait([&]{return deliveries==9;}));assert(!delivered.discontinuity);
        frame.epoch=91;frame.sequence=1;send_frame(server,frame);assert(wait([&]{return deliveries==10;}));
        subscriber.stop();
    }
}
static void publisher_provenance() {
    NetworkPublisher source,relay;assert(source.listen(QHostAddress::LocalHost,0));assert(relay.listen(QHostAddress::LocalHost,0));
    source.advertise("source",FrameType::Mel);relay.advertise("relay",FrameType::Mel);
    NetworkSubscriber input,sink;int ready=0;std::vector<TypedFrame> received;
    auto status=[&](QString value){if(value=="Running")++ready;};input.set_status_handler(status);sink.set_status_handler(status);
    input.set_frame_handler([&](TypedFrame frame){assert(relay.publish("relay",frame));});
    sink.set_frame_handler([&](TypedFrame frame){received.push_back(std::move(frame));});
    input.start("127.0.0.1",source.port(),"source",FrameType::Mel);
    sink.start("127.0.0.1",relay.port(),"relay",FrameType::Mel);assert(wait([&]{return ready==2;}));
    TypedFrame frame;frame.type=FrameType::Mel;frame.stream_id="caller";frame.epoch=frame.producer_epoch=1;
    frame.generation=1;frame.sequence=11;frame.sample_rate=48000;frame.timestamp_ns=-123;frame.source_sequence=45;frame.values={1,2,3};
    assert(source.publish("source",frame));assert(wait([&]{return received.size()==1;}));
    const auto old=received.back();assert(old.producer_epoch && old.generation==1 && old.timestamp_ns==-123 && old.source_sequence==45);
    TypedFrame unknown;unknown.type=FrameType::Mel;unknown.available=false;
    assert(source.publish("source",unknown));assert(wait([&]{return received.size()==2;}));
    assert(!received.back().available && received.back().producer_epoch==old.producer_epoch);
    frame.sequence=12;assert(source.publish("source",frame));assert(wait([&]{return received.size()==3;}));
    assert(received.back().producer_epoch==old.producer_epoch && received.back().discontinuity);
    // A publisher incarnation changes even when the local epoch/generation repeat.
    const auto port=source.port();source.stop();assert(source.listen(QHostAddress::LocalHost,port));
    assert(wait([&]{return ready==3;},3000));frame.sequence=1;
    assert(source.publish("source",frame));assert(wait([&]{return received.size()==4;}));
    const auto restarted=received.back();assert(restarted.epoch==old.epoch && restarted.producer_epoch!=old.producer_epoch);
    assert(restarted.sequence==1 && restarted.generation==1 && restarted.values==frame.values);
    frame.producer_epoch=2;frame.sequence=1;assert(source.publish("source",frame));assert(wait([&]{return received.size()==5;}));
    assert(received.back().producer_epoch!=restarted.producer_epoch);
    assert(frame.producer_epoch==2 && frame.epoch==1 && frame.stream_id=="caller");
    input.stop();sink.stop();source.stop();relay.stop();
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    subscriber_fences(app.arguments().contains("--sequence-fence-only"));
    publisher_provenance();
    TypedFrame golden;
    golden.type = FrameType::Mel;
    golden.sample_rate = 1;
    golden.sequence = 1;
    golden.values = {1};
    const auto expected =
        QByteArray::fromHex("020005000000000000000000000000000000000000000000000000000100000000000000000000000000000000"
                            "000000000000000000000000000000010000000000000000000000000000000000000000000000000000000100"
                            "000001000000000000000000000000000000000000000000803f");
    const auto encoded = stream::encode(golden);
    assert(QByteArray(reinterpret_cast<const char *>(encoded.data()), encoded.size()) == expected);
    TypedFrame malformed;
    std::string malformed_error;
    auto oversized = encoded;
    for (int i = 88; i < 92; ++i)
        oversized[i] = 255;
    assert(!stream::decode(oversized, malformed, malformed_error));
    for (int type = 0; type < 7; ++type) {
        TypedFrame f;
        f.type = FrameType(type);
        f.sample_rate = 48000;
        f.sequence = 17;
        f.generation = 2;
        f.timestamp_ns = -123;
        f.graph_revision = 3;
        f.epoch = 4;
        f.source_sequence = 123;
        f.producer_epoch = 99;
        f.stream_id = "test";
        f.discontinuity = true;
        if (type < 4) {
            f.left = {1, 2, 3};
            f.right = {4, 5, 6};
        } else if (type < 6)
            f.values = {1, 2, 3};
        else
            f.history = {{1, 2, 3}, {4, 5, 6}};
        auto bytes = stream::encode(f);
        TypedFrame result;
        std::string error;
        assert(stream::decode(bytes, result, error));
        assert(result.left == f.left && result.right == f.right && result.values == f.values &&
               result.history == f.history && result.timestamp_ns == -123 && result.graph_revision == 3 &&
               result.source_sequence == 123 && result.producer_epoch == 99);
        bytes[0] = 3;
        assert(!stream::decode(bytes, result, error) && error == "Unsupported stream version");
        bytes[0] = 2;
        bytes.pop_back();
        assert(!stream::decode(bytes, result, error));
    }
    TypedFrame unavailable;
    unavailable.available = false;
    unavailable.type = FrameType::History;
    unavailable.stream_id = "history";
    auto marker = stream::encode(unavailable);
    TypedFrame decoded_marker;
    assert(stream::decode(marker, decoded_marker, malformed_error));
    assert(!decoded_marker.available && decoded_marker.history.empty());
    marker[88] = 1;
    assert(!stream::decode(marker, decoded_marker, malformed_error));
    NetworkPublisher publisher;
    publisher.advertise("pcm", FrameType::PCM);
    assert(publisher.listen());
    NetworkSubscriber a, b;
    int ready = 0, countA = 0, countB = 0;
    bool gap = false;
    a.set_status_handler([&](QString s) {
        if (s == "Running")
            ++ready;
    });
    b.set_status_handler([&](QString s) {
        if (s == "Running")
            ++ready;
    });
    a.set_frame_handler([&](TypedFrame f) {
        ++countA;
        gap = f.discontinuity;
        assert(f.left.size() == 20000);
        assert(f.stream_id=="pcm");
    });
    b.set_frame_handler([&](TypedFrame) { ++countB; });
    a.start("127.0.0.1", publisher.port(), "pcm", FrameType::PCM);
    b.start("127.0.0.1", publisher.port(), "pcm", FrameType::PCM);
    assert(wait([&] { return ready == 2; }));
    TypedFrame f;
    f.sample_rate = 48000;
    f.left.resize(20000, 0.5f);
    f.right.resize(20000, -0.5f);
    f.sequence = 1;
    assert(publisher.publish("pcm", f));
    assert(f.stream_id.empty());
    assert(wait([&] { return countA == 1 && countB == 1; }));
    assert(gap);
    f.sequence = 2;
    f.stream_id="prior-stream";
    publisher.publish("pcm", f);
    assert(f.stream_id=="prior-stream");
    assert(wait([&] { return countA == 2 && countB == 2; }));
    assert(!gap);
    a.stop();
    f.sequence = 3;
    publisher.publish("pcm", f);
    assert(wait([&] { return countB == 3; }));
    assert(countA == 2);
    const auto endpoint = publisher.port();
    publisher.stop();
    assert(publisher.listen(QHostAddress::LocalHost, endpoint));
    assert(wait([&] { return ready == 3; }, 3000));
    f.sequence = 1;
    publisher.publish("pcm", f);
    assert(wait([&] { return countB == 4; }));
    // A subscriber which stops reading cannot hold up the healthy subscriber.
    TcpBDSPSocket slow;
    slow.setReadBufferSize(128);
    slow.connectToHost(QHostAddress::LocalHost, endpoint);
    assert(wait([&] { return slow.state() == QAbstractSocket::ConnectedState; }));
    QByteArray request;
    request.append(char(0));
    request.append(char(2));
    request.append(char(0));
    request.append("pcm");
    assert(slow.send_packet(11, request));
    QElapsedTimer settle;
    settle.start();
    wait([&] { return settle.elapsed() > 50; });
    const int initial = countB;
    for (int i = 0; i < 160; ++i) {
        f.sequence = 2 + i;
        publisher.publish("pcm", f);
        wait([&] { return countB > initial + i; }, 30);
    }
    assert(countB > initial + 100);
    assert(publisher.dropped() > 0);
    bool cleared = false, revived = false;
    b.set_frame_handler([&](TypedFrame received) {
        if (!received.available)
            cleared = true;
        else if (cleared)
            revived = true;
    });
    TypedFrame clear;
    clear.available = false;
    clear.type = FrameType::PCM;
    assert(publisher.publish("pcm", clear));
    assert(wait([&] { return cleared; }));
    settle.restart();
    wait([&] { return settle.elapsed() > 50; });
    assert(!revived); // All queued old data precedes the clear marker.
    bool resumed = false;
    b.set_frame_handler([&](TypedFrame received) { resumed = received.available && received.discontinuity; });
    f.sequence = 200;
    publisher.publish("pcm", f);
    assert(wait([&] { return resumed; }));
    slow.abort();
    b.stop();
    NetworkSubscriber missing;
    bool refused = false;
    missing.set_status_handler([&](QString s) {
        if (s.contains("Unsupported"))
            refused = true;
    });
    missing.start("127.0.0.1", endpoint, "missing", FrameType::PCM);
    assert(wait([&] { return refused; }));
    missing.stop();
    TcpBDSPSocket unsupported;
    bool error_reply = false;
    QObject::connect(&unsupported, &TcpBDSPSocket::packetReceived, &app, [&](quint8 id, QByteArray) {
        if (id == 14)
            error_reply = true;
    });
    unsupported.connectToHost(QHostAddress::LocalHost, endpoint);
    assert(wait([&] { return unsupported.state() == QAbstractSocket::ConnectedState; }));
    request[1] = 3;
    unsupported.send_packet(11, request);
    assert(wait([&] { return error_reply; }));
    QTcpServer silent;
    assert(silent.listen(QHostAddress::LocalHost, 0));
    NetworkSubscriber timed;
    bool timed_out = false;
    timed.set_status_handler([&](QString status) {
        if (status == "Recovering")
            timed_out = true;
    });
    timed.start("127.0.0.1", silent.serverPort(), "pcm", FrameType::PCM);
    assert(wait([&] { return timed_out; }, 6000));
    timed.stop();
    std::cout << "codec types, fragmented multicast, cancellation passed\n";
}
