#include "pipeline/engine_client.h"
#include "pipeline/stream_codec.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTcpServer>
#include <QElapsedTimer>
#include <QDataStream>
#include <QJsonArray>
#include "core/network/tcp_packet_socket.h"
#include <stdexcept>
#include <thread>
#include <cmath>
#include <mutex>
#include <atomic>
// A scripted peer supplies deliberately reordered protocol-v1 snapshots/replies.
class ScriptedEngineServer final : public QTcpServer {
public:
    TcpBDSPSocket *peer=nullptr;
    QList<quint64> requests;
protected:
    void incomingConnection(qintptr descriptor) override {
        peer=new TcpBDSPSocket(this);
        if (!peer->setSocketDescriptor(descriptor)) throw std::runtime_error("Scripted peer descriptor failed");
        QObject::connect(peer,&TcpBDSPSocket::packetReceived,this,[this](quint8 id,const QByteArray &bytes){
            if(id!=5) return;
            QDataStream stream(bytes); stream.setVersion(QDataStream::Qt_6_0); stream.setByteOrder(QDataStream::LittleEndian);
            quint16 version=0; quint64 request=0; stream>>version>>request;
            if(version==1&&stream.status()==QDataStream::Ok) requests.append(request);
        });
    }
};
static QByteArray scripted_snapshot(const pipeline::EngineSnapshot &snapshot, quint64 reply=0) {
    QByteArray bytes;
    QDataStream stream(&bytes,QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0); stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream<<quint16(1);
    if(reply) stream<<reply<<true<<QString{};
    stream<<snapshot.epoch<<snapshot.revision<<snapshot.graph_revision<<snapshot.analyzer_generation<<snapshot.capture_generation
          <<QByteArray("{}")<<quint32(1);
    const auto status=snapshot.statuses.value("Analyzer");
    stream<<QString("Analyzer")<<quint8(status.state)<<status.error<<quint64(status.revision)<<bool(snapshot.history);
    if(snapshot.history) {
        stream<<quint64(snapshot.history->graph_revision)<<quint32(snapshot.history->values.size());
        for(const auto &row:snapshot.history->values) {
            stream<<quint32(row.size()); for(float value:row) stream<<value;
        }
    }
    return bytes;
}
static QByteArray scripted_optional_view(const pipeline::EngineSnapshot &snapshot) {
    QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream<<quint16(1)<<snapshot.epoch<<snapshot.graph_revision<<quint64(1)<<false
          <<quint64(1)<<qint64(1)<<qint64(1)<<quint64(1)<<float(.1)<<quint64(48000)
          <<snapshot.analyzer_generation<<false<<quint64(0);
    for(int i=0;i<9;++i) {
        stream<<quint32(i>=4?1:0);
        if(i>=4)stream<<float(i==4?11:i==5?22:123+i-6);
    }
    stream<<float(1)<<quint32(1)<<qint32(9)<<quint64(1)<<float(123)
          <<true<<snapshot.graph_revision<<quint32(1)<<quint32(1)<<float(123);
    return bytes;
}
static QByteArray scripted_typed_view(const pipeline::EngineSnapshot &snapshot, bool available=true) {
    pipeline::TypedFrame frame;frame.type=pipeline::FrameType::Mel;frame.sample_rate=48000;frame.bands=3;
    frame.epoch=snapshot.epoch;frame.graph_revision=snapshot.graph_revision;
    frame.generation=77;frame.producer_epoch=77;frame.sequence=available?1:0;frame.available=available;
    if(available)frame.values={1,2,3};
    const auto payload=pipeline::stream::encode(frame);
    if(payload.empty())throw std::runtime_error("Scripted typed frame encoding failed");
    QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);stream.setByteOrder(QDataStream::LittleEndian);
    stream<<quint16(2)<<snapshot.epoch<<snapshot.graph_revision<<quint32(1)<<qint32(1)<<quint32(payload.size());
    bytes.append(reinterpret_cast<const char *>(payload.data()),payload.size());return bytes;
}
static void require(bool x, const char *message) {
    if (!x)
        throw std::runtime_error(message);
}
static void pump(int ms = 30) {
    QEventLoop l;
    QTimer::singleShot(ms, &l, &QEventLoop::quit);
    l.exec();
}
template <class F> static bool wait_until(F f, int timeout = 1500) {
    QElapsedTimer t;
    t.start();
    while (!f() && t.elapsed() < timeout)
        pump(20);
    return f();
}
struct FakeSourceState {
    std::atomic<size_t> produced{0}, stopped{0};
    std::vector<pipeline::AudioSourceKind> sources;
};
class FakeAudioSource final : public pipeline::IAudioSource {
public:
    FakeAudioSource(pipeline::AudioSourceKind kind, std::shared_ptr<FakeSourceState> state, bool deliver_audio=true)
        : kind(kind), state(std::move(state)), deliver_audio(deliver_audio) {
        this->state->sources.push_back(kind);
    }
    ~FakeAudioSource() override { stop(); }
    void start() override {
        if (running.exchange(true)) return;
        if (status) status(true, {});
        if (lifecycle) lifecycle(true, 0, {});
        producer = std::thread([this] {
            size_t offset = 0;
            while (running) {
                pipeline::AudioFrame frame; frame.sample_rate = 48000;
                frame.left.resize(4096); frame.right.resize(4096);
                for(size_t i=0;i<4096;++i) {
                    float sample=0;
                    for(int tone=0;tone<8;++tone)
                        sample+=std::sin(float(offset+i)*float(2*3.141592653589793*(200+tone*600+(offset/4096)%7*11)/48000))/32;
                    frame.left[i]=frame.right[i]=sample;
                }
                offset += 4096;
                if (audio && deliver_audio) audio(std::move(frame));
                ++state->produced;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
    void stop() override {
        if (!running.exchange(false)) return;
        if (producer.joinable()) producer.join();
        ++state->stopped;
        if (lifecycle) lifecycle(false, 0, {});
    }
    bool is_running() const override { return running.load(); }
    pipeline::AudioSourceKind source() const override { return kind; }
    void set_audio_handler(AudioHandler h) override { audio=std::move(h); }
    void set_status_handler(StatusHandler h) override { status=std::move(h); }
    void set_lifecycle_handler(LifecycleHandler h) override { lifecycle=std::move(h); }
private:
    pipeline::AudioSourceKind kind;
    std::shared_ptr<FakeSourceState> state;
    std::atomic<bool> running{false};
    std::thread producer;
    AudioHandler audio; StatusHandler status; LifecycleHandler lifecycle;
    bool deliver_audio;
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        {
            pipeline::EngineClient client([](pipeline::AudioSourceKind){return std::unique_ptr<pipeline::IAudioSource>{};});
            auto *engine=client.local_engine();
            QString error;
            QJsonObject graph{{"version",2},{"nodes",QJsonArray{
                QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","mel"},{"stream_id","mel"}}}},
                QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","MelView"}}}}}},
                {"connections",QJsonArray{QJsonObject{{"outNodeId",0},{"inNodeId",1},{"outPortIndex",0},{"inPortIndex",0}}}}};
            require(engine->apply_graph(graph,error),"Local subscription probe graph failed");
            pump();
            int delivered=0;bool typed_fields=false;
            client.set_view_handler([&](auto view){++delivered;typed_fields=!view.views.isEmpty();});
            quint64 acknowledged=0;bool accepted=false;
            client.set_reply_handler([&](auto reply){acknowledged=reply.request_id;accepted=reply.accepted;});
            pipeline::TypedFrame frame;frame.type=pipeline::FrameType::Mel;frame.sample_rate=48000;frame.values={1,2,3};frame.sequence=1;
            engine->feed_typed(0,frame); // pending in the owner-thread mailbox
            pipeline::EngineCommand subscription;subscription.kind=pipeline::EngineCommand::Subscribe;subscription.subscriptions=0;
            const auto unsubscribe=client.submit(subscription);pump(40);
            require(acknowledged==unsubscribe && accepted,"Local unsubscribe was not acknowledged");
            require(delivered==0,"Queued local view escaped Subscribe(0)");
            frame.sequence=2;engine->feed_typed(0,frame);pump(40);
            require(delivered==0,"Subsequent local view escaped Subscribe(0)");
            subscription.subscriptions=pipeline::EngineCommand::Views|pipeline::EngineCommand::TypedViews;
            client.submit(subscription);pump();
            frame.sequence=3;engine->feed_typed(0,frame);
            subscription.subscriptions=0;client.submit(subscription);
            subscription.subscriptions=pipeline::EngineCommand::Views|pipeline::EngineCommand::TypedViews;client.submit(subscription);
            pump(40);require(delivered==0,"Old local mailbox survived unsubscribe/resubscribe");
            frame.sequence=4;engine->feed_typed(0,frame);
            require(wait_until([&]{return delivered==1;}),"Current local subscription did not resume");
            require(typed_fields,"Current typed local subscription lost typed views");
            frame.sequence=5;engine->feed_typed(0,frame);
            subscription.subscriptions=pipeline::EngineCommand::Views;client.submit(subscription);pump(40);
            require(delivered==1,"Queued typed view escaped local legacy selection");
            frame.sequence=6;engine->feed_typed(0,frame);
            require(wait_until([&]{return delivered==2;}),"Local legacy view subscription did not resume");
            require(!typed_fields,"Local legacy subscription retained typed view fields");
        }
        auto local_source_state=std::make_shared<FakeSourceState>();
        pipeline::EngineClient local([local_source_state](pipeline::AudioSourceKind kind) {
            return std::make_unique<FakeAudioSource>(kind,local_source_state,false);
        });
        int replies = 0;
        pipeline::EngineReply last;
        local.set_reply_handler([&](auto r) {
            last = std::move(r);
            ++replies;
        });
        pipeline::EngineCommand c;
        c.kind = pipeline::EngineCommand::SetDisplayChannel;
        c.value = 2;
        local.submit(c);
        require(replies == 0, "Local command completed synchronously");
        pump();
        require(replies == 1 && last.accepted, "Local async command failed");
        require(!local_source_state->sources.empty(),"EngineClient ignored its injected capture factory");
        c.expected_revision = 0;
        local.submit(c);
        pump();
        require(!last.accepted && last.error == "Revision conflict", "Stale mutation accepted");
        // Status delivery can precede a command reply containing an older status.
        quint64 status_revision=0;bool regressed=false;
        local.set_snapshot_handler([&](auto v){const auto r=v.statuses.value("Analyzer").revision;if(r<status_revision)regressed=true;status_revision=std::max<quint64>(status_revision,r);});
        for(int i=0;i<20;++i){pipeline::EngineCommand start;start.kind=pipeline::EngineCommand::StartAnalyzer;local.submit(start);require(wait_until([&]{return local.snapshot().statuses.value("Analyzer").state==pipeline::State::Running;}),"Async lifecycle never reached running");start.kind=pipeline::EngineCommand::StopAnalyzer;local.submit(start);require(wait_until([&]{return local.snapshot().statuses.value("Analyzer").state==pipeline::State::Stopped;}),"Async lifecycle never reached stopped");}
        require(!regressed,"A command reply regressed a newer lifecycle snapshot");
        {
            auto *engine=local.local_engine();
            engine->start_analyzer();
            pipeline::AudioFrame seed; seed.sample_rate=48000;
            seed.left.assign(4096,0.25f); seed.right=seed.left;
            engine->feed(seed);
            require(wait_until([&]{return engine->history_count()>0;}),"Local history seed failed");
            require(wait_until([&]{local.submit(pipeline::EngineCommand{});return local.snapshot().history&&
                !local.snapshot().history->values.empty();}),"Local cache history seed failed");
            const auto old_generation=engine->analyzer_generation();
            bool incompatible=false;
            local.set_snapshot_handler([&](auto v){
                if(v.history&&(v.history->analyzer_generation!=v.analyzer_generation||
                              v.history->graph_revision!=v.graph_revision||
                              v.statuses.value("Analyzer").state!=pipeline::State::Running)) incompatible=true;
            });
            // Coalesced status is queued before the reply, then sees a later restart.
            engine->stop_wled();
            const auto delayed=local.submit(pipeline::EngineCommand{});
            const auto previous_replies=replies;
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            require(replies==previous_replies,"Local reply escaped the owner event loop");
            engine->stop_analyzer(); engine->start_analyzer();
            for(int i=0;i<100&&engine->current_status("Analyzer").state!=pipeline::State::Running;++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            require(wait_until([&]{return last.request_id==delayed;}),"Delayed local reply missing");
            require(!incompatible&&local.snapshot().analyzer_generation>old_generation&&
                    !local.snapshot().history,"Local status/reply relabelled old history after restart");
            engine->stop_analyzer(); pump();
            local.set_snapshot_handler({});
        }
        {
            ScriptedEngineServer scripted;
            require(scripted.listen(QHostAddress::LocalHost,0),"Scripted server listen failed");
            pipeline::EngineClient client("127.0.0.1",scripted.serverPort());
            require(wait_until([&]{return scripted.requests.size()>=2;}),"Scripted handshake commands missing");
            pipeline::EngineSnapshot old;
            old.epoch=42; old.revision=1; old.graph_revision=7; old.analyzer_generation=1;
            old.statuses["Analyzer"]={pipeline::State::Running,{},1};
            auto history=std::make_shared<pipeline::HistorySnapshot>();
            history->graph_revision=7; history->analyzer_generation=1; history->values={{111}}; old.history=history;
            auto send=[&](const auto &snapshot,quint64 request=0){
                require(scripted.peer->send_packet(request?6:7,scripted_snapshot(snapshot,request)),"Scripted snapshot send failed");
            };
            send(old);
            require(wait_until([&]{return client.snapshot().history!=nullptr;}),"Remote history seed missing");
            require(client.snapshot().history->analyzer_generation==1,"V1 history lost enclosing generation");
            const auto delayed=client.submit(pipeline::EngineCommand{});
            require(wait_until([&]{return scripted.requests.contains(delayed);}),"Delayed remote command missing");
            auto restarted=old; restarted.revision=3; restarted.analyzer_generation=3;
            restarted.statuses["Analyzer"].revision=3; restarted.history.reset();
            send(restarted);
            require(wait_until([&]{return client.snapshot().analyzer_generation==3;}),"Remote restart missing");
            require(!client.snapshot().history,"Remote restart retained incompatible cached history");
            bool reply_received=false;
            client.set_reply_handler([&](auto reply){if(reply.request_id==delayed){reply_received=true;
                require(!reply.snapshot.history&&reply.snapshot.analyzer_generation==3,"Old reply relabelled remote history");}});
            send(old,delayed);
            require(wait_until([&]{return reply_received;}),"Reordered remote reply missing");
            auto current_history=std::make_shared<pipeline::HistorySnapshot>(*history);
            current_history->analyzer_generation=3; current_history->values={{333}};
            restarted.history=current_history; send(restarted);
            require(wait_until([&]{return client.snapshot().history!=nullptr;}),"Current remote history missing");
            send(old); pump();
            require(client.snapshot().history&&client.snapshot().history->values[0][0]==333,"Old snapshot replaced current history");
            auto changed=restarted; changed.revision=4; changed.graph_revision=8; changed.history.reset();
            send(changed);
            require(wait_until([&]{return client.snapshot().graph_revision==8;}),"Remote graph change missing");
            require(!client.snapshot().history,"Graph change retained incompatible cached history");
            send(restarted); pump();
            require(!client.snapshot().history&&client.snapshot().graph_revision==8,"Old graph snapshot restored stale history");
            auto stopped_history=std::make_shared<pipeline::HistorySnapshot>(*current_history);
            stopped_history->graph_revision=8;
            changed.statuses["Analyzer"]={pipeline::State::Stopped,{},5}; changed.history=stopped_history;
            send(changed); pump();
            require(!client.snapshot().history,"Stopped snapshot retained history");
            // A queued typed packet from the previous subscription must not be
            // presented as an empty legacy frame after switching subscriptions.
            changed.statuses["Analyzer"]={pipeline::State::Running,{},6};changed.revision=6;changed.history.reset();
            send(changed);
            require(wait_until([&]{return client.snapshot().statuses.value("Analyzer").state==pipeline::State::Running;}),
                    "Typed subscription probe did not resume running");
            const auto typed=scripted_typed_view(changed);
            size_t typed_deliveries=0;
            client.set_view_handler([&](auto){++typed_deliveries;});
            pipeline::EngineCommand subscription;subscription.kind=pipeline::EngineCommand::Subscribe;
            subscription.subscriptions=pipeline::EngineCommand::Views;
            client.submit(subscription);
            require(scripted.peer->send_packet(17,typed),"Scripted queued typed packet");pump();
            require(typed_deliveries==0,"Old TypedViews subscription escaped legacy selection");
            subscription.subscriptions=pipeline::EngineCommand::Views|pipeline::EngineCommand::TypedViews;
            client.submit(subscription);
            require(scripted.peer->send_packet(17,typed),"Scripted current typed packet");
            require(wait_until([&]{return typed_deliveries==1;}),"Current TypedViews subscription was suppressed");
            subscription.subscriptions=0;client.submit(subscription);
            require(scripted.peer->send_packet(17,typed),"Scripted unsubscribed typed packet");pump();
            require(typed_deliveries==1,"Unsubscribed TypedViews packet was delivered");
            subscription.subscriptions=pipeline::EngineCommand::Views|pipeline::EngineCommand::TypedViews;
            client.submit(subscription);
            changed.statuses["Analyzer"]={pipeline::State::Stopped,{},7};changed.revision=7;send(changed);
            require(wait_until([&]{return client.snapshot().statuses.value("Analyzer").state==pipeline::State::Stopped;}),
                    "Explicit remote Stop snapshot missing");
            require(scripted.peer->send_packet(17,typed),"Delayed valid pre-Stop typed packet send failed");pump();
            require(typed_deliveries==1,"Delayed TypedViews packet escaped confirmed Analyzer Stop");
            changed.statuses["Analyzer"]={pipeline::State::Recovering,{},8};changed.revision=8;send(changed);
            require(wait_until([&]{return client.snapshot().statuses.value("Analyzer").state==pipeline::State::Recovering;}),
                    "Remote recovery snapshot missing");
            bool unavailable=false;
            client.set_view_handler([&](auto view){++typed_deliveries;unavailable=!view.views.value(1).available;});
            require(scripted.peer->send_packet(17,scripted_typed_view(changed,false)),"Current unavailable typed marker send failed");
            require(wait_until([&]{return typed_deliveries==2;}),"Recovery unavailable marker was suppressed");
            require(unavailable,"Current unavailable typed marker lost its semantic state");
            client.disconnect_remote();
        }
        {
            ScriptedEngineServer peer;
            require(peer.listen(QHostAddress::LocalHost,0),"Optional-field peer listen failed");
            pipeline::EngineClient client("127.0.0.1",peer.serverPort());
            require(wait_until([&]{return peer.requests.size()>=2;}),"Optional-field handshake missing");
            pipeline::EngineSnapshot snapshot;snapshot.epoch=42;snapshot.revision=1;snapshot.graph_revision=7;
            snapshot.analyzer_generation=1;snapshot.statuses["Analyzer"]={pipeline::State::Running,{},1};
            auto seed=std::make_shared<pipeline::HistorySnapshot>();seed->graph_revision=7;seed->analyzer_generation=1;
            seed->values={{777}};snapshot.history=seed;
            require(peer.peer->send_packet(7,scripted_snapshot(snapshot)),"Optional-field snapshot send failed");
            require(wait_until([&]{return client.snapshot().epoch==42;}),"Optional-field snapshot missing");
            using C=pipeline::EngineCommand;
            C subscribe;subscribe.kind=C::Subscribe;subscribe.subscriptions=C::Views|C::History|C::Previews|C::Diagnostics;
            client.submit(subscribe);pump();
            int deliveries=0;pipeline::ViewFrame delivered;
            client.set_view_handler([&](auto frame){++deliveries;delivered=std::move(frame);});
            const auto packet=scripted_optional_view(snapshot);
            require(peer.peer->send_packet(8,packet),"Buffered old optional view send failed");
            subscribe.subscriptions=C::Views;client.submit(subscribe);
            require(wait_until([&]{return deliveries==1;}),"Current legacy base view suppressed");
            require(delivered.window_signal.empty() && delivered.window.empty() && delivered.windowed_signal.empty() &&
                    delivered.previews.isEmpty() && !delivered.history,"Remote legacy view retained disabled optional fields");
            require(delivered.fft==std::vector<float>{11} && delivered.mel==std::vector<float>{22},"Optional filtering changed base values");
            require(client.snapshot().history && client.snapshot().history->values==seed->values,
                    "Disabled legacy History repopulated the snapshot cache");
            for(const auto flags:{C::History,C::Previews,C::Diagnostics}) {
                subscribe.subscriptions=C::Views|flags;client.submit(subscribe);
                const auto before=deliveries;
                require(peer.peer->send_packet(8,packet),"Selected optional view send failed");
                require(wait_until([&]{return deliveries==before+1;}),"Selected optional view missing");
                require(bool(delivered.history)==bool(flags&C::History) &&
                        !delivered.previews.isEmpty()==bool(flags&C::Previews) &&
                        !delivered.window_signal.empty()==bool(flags&C::Diagnostics),"Optional flags did not select exact fields");
                if(flags&C::History)require(delivered.history->values==std::vector<std::vector<float>>{{123}},"Selected history payload changed");
                if(flags&C::Previews)require(delivered.previews.value(9).peak==123,"Selected preview payload changed");
                if(flags&C::Diagnostics)require(delivered.window_signal==std::vector<float>{123} &&
                    delivered.window==std::vector<float>{124} && delivered.windowed_signal==std::vector<float>{125},"Selected diagnostic payload changed");
            }
            // A protocol-v1 peer remains a valid fallback when TypedViews is requested.
            subscribe.subscriptions=C::Views|C::TypedViews;client.submit(subscribe);
            auto before=deliveries;require(peer.peer->send_packet(8,packet),"Legacy fallback send failed");
            require(wait_until([&]{return deliveries==before+1;}),"Protocol-v1 legacy fallback was suppressed");
            subscribe.subscriptions=0;client.submit(subscribe);before=deliveries;
            require(peer.peer->send_packet(8,packet),"Unsubscribed legacy send failed");pump();
            require(deliveries==before,"Unsubscribed legacy packet was delivered");
            client.disconnect_remote();
        }
        auto source_state=std::make_shared<FakeSourceState>();
        {pipeline::AudioPipeline injected([source_state](pipeline::AudioSourceKind kind){return std::make_unique<FakeAudioSource>(kind,source_state);});std::atomic<size_t> raw{0};injected.set_audio_handler([&](auto f){require(f.sample_rate==48000 && f.left.size()==4096 && f.right.size()==4096,"Capture port changed owned audio");++raw;});injected.start_analyzer();injected.start_capture();require(injected.capture_diagnostics().backend.isEmpty(),"Custom source diagnostics changed");require(wait_until([&]{return injected.history_count()>0 && raw.load()>2;}),"Custom source did not feed the same DSP history");injected.stop_capture();const auto stopped=raw.load();pump(40);require(raw.load()==stopped,"Custom source callback escaped stop cancellation");QString error;require(injected.apply_graph(pipeline::GraphProgram::defaults(0,SpectrumMerge::Energy,1),error),"Custom source graph failed");injected.start_capture();require(wait_until([&]{return raw.load()>stopped;}),"Custom microphone port did not restart");injected.stop_capture();injected.stop_analyzer();}
        require(source_state->stopped==2 && source_state->sources.size()==2 && source_state->sources.back()==pipeline::AudioSourceKind::Microphone,"Audio source factory or stop contract violated");
        auto transport_source_state=std::make_shared<FakeSourceState>();
        pipeline::AudioPipeline engine([transport_source_state](pipeline::AudioSourceKind kind) {
            return std::make_unique<FakeAudioSource>(kind,transport_source_state,false);
        });
        pipeline::EngineServer server(engine);
        require(server.listen(QHostAddress::LocalHost, 0, false), "Listen failed");
        pipeline::EngineClient remote("127.0.0.1", server.port());
        int snapshots = 0;
        remote.set_snapshot_handler([&](auto) { ++snapshots; });
        remote.set_reply_handler([&](auto r) { last = std::move(r); });
        pump(100);
        require(snapshots > 0 && remote.snapshot().epoch != 0, "Remote handshake failed");
        remote.submit(c);
        pump();
        require(!last.accepted && last.error == "Server is read-only", "Read-only server accepted mutation");
        remote.disconnect_remote();
        pump();
        remote.connect_remote();
        pump(100);
        require(remote.snapshot().epoch != 0, "Reconnect failed");
        pipeline::EngineServer writable(engine);
        require(writable.listen(QHostAddress::LocalHost, 0, true), "Writable listen failed");
        pipeline::EngineClient control("127.0.0.1", writable.port());
        control.set_reply_handler([&](auto r) { last = std::move(r); });
        pump(100);
        c.expected_revision = control.snapshot().revision;
        control.submit(c);
        pump();
        require(last.accepted, "TCP mutation failed");
        // Large owned control documents must survive BDSP uint16 fragmentation.
        c.kind = pipeline::EngineCommand::ApplyGraph;
        c.expected_revision = control.snapshot().revision;
        c.graph = pipeline::GraphProgram::defaults();
        c.graph["annotation"] = QString(140000, 'x');
        control.submit(c);
        pump(100);
        require(last.accepted && control.snapshot().graph.value("annotation").toString().size() == 140000,
                "Fragmented graph lost data");
        // Parser failures must close only the offending client.
        auto malformed = [&](quint8 id, const QByteArray &payload) {
            TcpBDSPSocket bad;
            bad.connectToHost("127.0.0.1", writable.port());
            pump();
            require(bad.state() == QAbstractSocket::ConnectedState, "Malformed probe connection failed");
            bad.send_data(id, reinterpret_cast<uint8_t *>(const_cast<char *>(payload.data())), payload.size());
            pump();
            require(bad.state() == QAbstractSocket::UnconnectedState, "Malformed packet left connection alive");
        };
        QByteArray invalid;
        QDataStream wrong(&invalid, QIODevice::WriteOnly);
        wrong.setByteOrder(QDataStream::LittleEndian);
        wrong << quint16(99);
        malformed(5, invalid);
        invalid.clear();
        QDataStream fragment(&invalid, QIODevice::WriteOnly);
        fragment.setVersion(QDataStream::Qt_6_0);
        fragment.setByteOrder(QDataStream::LittleEndian);
        fragment << quint16(1) << quint8(5) << quint64(1) << quint32(0xffffffff) << quint32(0) << QByteArray("x");
        malformed(10, invalid);
        invalid.clear();
        QDataStream huge(&invalid, QIODevice::WriteOnly);
        huge.setVersion(QDataStream::Qt_6_0);
        huge.setByteOrder(QDataStream::LittleEndian);
        huge << quint16(1) << quint64(123) << quint8(0) << quint64(0) << qint32(0) << quint32(1) << quint32(0x7fffffff);
        malformed(5, invalid);
        require(control.snapshot().epoch != 0, "Malformed peer affected healthy client");
        std::vector<std::unique_ptr<TcpBDSPSocket>> peers;
        for (int i = 0; i < 8; ++i) {
            auto peer = std::make_unique<TcpBDSPSocket>();
            peer->connectToHost("127.0.0.1", writable.port());
            peers.push_back(std::move(peer));
        }
        pump(100);
        size_t connected = 0;
        for (auto &peer : peers)
            if (peer->state() == QAbstractSocket::ConnectedState)
                ++connected;
        require(connected == 7, "Server client limit is not bounded at eight");
        // Fragment bytes are owned per peer; disconnect cancels its incomplete assembly.
        invalid.clear();
        QDataStream partial(&invalid, QIODevice::WriteOnly);
        partial.setVersion(QDataStream::Qt_6_0);
        partial.setByteOrder(QDataStream::LittleEndian);
        partial << quint16(1) << quint8(5) << quint64(9) << quint32(70000) << quint32(0) << QByteArray("fragment");
        peers.front()->send_data(10, reinterpret_cast<uint8_t *>(invalid.data()), invalid.size());
        pump();
        peers.front()->abort();
        pump();
        control.submit(pipeline::EngineCommand{});
        pump();
        require(last.accepted, "Partial peer corrupted healthy command stream");
        size_t views = 0;
        quint64 last_sequence = 0;
        control.set_view_handler([&](pipeline::ViewFrame v) {
            require(QThread::currentThread()==app.thread(),"Remote view callback ran outside client Qt thread");
            require(v.epoch == control.snapshot().epoch, "Stale epoch view delivered");
            require(v.history == nullptr || v.history->graph_revision == v.graph_revision, "Stale history delivered");
            ++views;
            last_sequence = v.spectrum.sequence;
        });
        c = pipeline::EngineCommand{};
        c.kind = pipeline::EngineCommand::Subscribe;
        c.subscriptions = pipeline::EngineCommand::Views | pipeline::EngineCommand::Diagnostics;
        control.submit(c);
        pump();
        const auto views_before_block=views;
        engine.start_analyzer();
        pipeline::AudioFrame input;
        input.sample_rate = 48000;
        input.left.resize(4096);
        input.right.resize(4096);
        for (size_t i = 0; i < input.left.size(); ++i) {
            float sample=0;
            for(int tone=0;tone<8;++tone) sample+=std::sin(float(i)*float(2*3.141592653589793*(200+tone*600)/48000))/32;
            input.left[i]=input.right[i]=sample;
        }
        engine.feed(input);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(views == views_before_block, "Remote view callback ran outside client event loop");
        require(wait_until([&] { return views > views_before_block; }), "Subscribed view stream timed out");
        require(views > 0 && last_sequence > 0, "Subscribed view stream missing");
        engine.stop_analyzer();
        require(wait_until([&] {
            const auto snapshot=control.snapshot();
            return snapshot.statuses.value("Analyzer").state==pipeline::State::Stopped &&
                   snapshot.analyzer_generation==engine.analyzer_generation();
        }),"Remote Stop snapshot timed out");
        const auto stopped_views = views;
        engine.feed(input);
        pump();
        require(views == stopped_views, "Stopped analyzer emitted stale remote view");
        engine.start_analyzer();
        engine.feed(input);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        engine.stop_analyzer();
        pump(100);
        require(views == stopped_views, "Stopped generation escaped queue filtering");
        // Producer callbacks may have copied delivery handles when the server retires.
        {
            pipeline::AudioPipeline detached;
            detached.start_analyzer();
            {
                pipeline::EngineServer retiring(detached);
                detached.feed(input);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            detached.feed(input);
            pump(100);
            detached.stop_analyzer();
        }
        QTcpServer silent;
        require(silent.listen(QHostAddress::LocalHost, 0), "Silent peer listen failed");
        pipeline::EngineClient hanging("127.0.0.1", silent.serverPort());
        bool timed_out = false, false_ready = false;
        hanging.set_reply_handler([&](auto r) {
            if (r.error == "Command timed out")
                timed_out = true;
        });
        hanging.set_snapshot_handler([&](auto v) {
            if (v.statuses.value("Connection").state == pipeline::State::Running)
                false_ready = true;
        });
        require(wait_until([&] { return timed_out; }, 6500), "Silent peer never timed out");
        require(!false_ready, "TCP connection marked running before engine handshake");
        hanging.disconnect_remote();
        qInfo("Engine transport checks passed");
        return 0;
    } catch (const std::exception &e) {
        qCritical("%s", e.what());
        return 1;
    }
}
