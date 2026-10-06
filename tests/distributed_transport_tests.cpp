#include "pipeline/stream_transport.h"
#include "pipeline/engine_client.h"
#include "pipeline/test_signal.h"
#include "aggregator/aggregator_server.h"
#include "aggregator/aggregator_client.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QThread>
#include <QTcpServer>
#include <cmath>
#include <stdexcept>
using namespace pipeline;
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
static bool wait(std::function<bool()> f, int ms = 5000) {
    QElapsedTimer t;
    t.start();
    while (!f() && t.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return f();
}
static int port() {
    QTcpServer s;
    require(s.listen(QHostAddress::LocalHost, 0), "reserve port");
    return s.serverPort();
}
static QJsonObject graph(QStringList kinds, QMap<int, QJsonObject> params = {}) {
    QJsonArray nodes, edges;
    for (int i = 0; i < kinds.size(); ++i) {
        auto p = params.value(i);
        p["model-name"] = kinds[i];
        nodes.append(QJsonObject{{"id", i}, {"internal-data", p}});
        if (i)
            edges.append(QJsonObject{{"outNodeId", i - 1}, {"outPortIndex", 0}, {"inNodeId", i}, {"inPortIndex", 0}});
    }
    return {{"version", 2}, {"nodes", nodes}, {"connections", edges}};
}
static void compare(const TypedFrame &a, const TypedFrame &b) {
    require(a.history.size() == b.history.size(), "History rows mismatch");
    for (size_t r = 0; r < a.history.size(); ++r) {
        require(a.history[r].size() == b.history[r].size(), "History columns mismatch");
        for (size_t c = 0; c < a.history[r].size(); ++c)
            require(std::abs(a.history[r][c] - b.history[r][c]) < 1e-5f, "Distributed numeric mismatch");
    }
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QProcess processes[3];
    try {
        QTemporaryDir dir;
        require(dir.isValid(), "temp dir");
        AggregatorServer aggregator;
        require(aggregator.start(0), "aggregator start");
        int ap = port(), bp = port(), cp = port(), control = port();
        QString uuids[3];
        for (auto &u : uuids)
            u = QUuid::createUuid().toString(QUuid::WithoutBraces);
        AggregatorClient observer({QUuid::createUuid(), ComponentType::data_sink, 0});
        observer.connect_to_aggregator_server(QHostAddress::LocalHost, aggregator.serverPort(), 2000);
        require(wait([&] { return observer.is_registered(); }), "observer registration");
        auto tones = TestSignalSettings{}.json();
        tones["amplitude"] = 0.04;
        auto a =
            graph({"TestSignal", "NetworkOutput"}, {{0, tones}, {1, {{"data_type", "audio"}, {"stream_id", "pcm"}}}});
        auto b = graph({"NetworkInput", "Window", "WindowFunction", "StereoFFT", "BarkWeighting", "SpectrumMerge",
                        "Mel", "NetworkOutput"},
                       {{0, {{"data_type", "audio"}, {"stream_id", "pcm"}, {"provider_uuid", uuids[0]}}},
                        {7, {{"data_type", "mel"}, {"stream_id", "mel"}}}});
        auto branchNodes = b["nodes"].toArray();
        auto branchEdges = b["connections"].toArray();
        auto independentTone = tones;
        independentTone["model-name"] = "TestSignal";
        branchNodes.append(QJsonObject{{"id", 100}, {"internal-data", independentTone}});
        branchNodes.append(QJsonObject{
            {"id", 101},
            {"internal-data",
             QJsonObject{{"model-name", "NetworkOutput"}, {"data_type", "audio"}, {"stream_id", "independent"}}}});
        branchEdges.append(QJsonObject{{"outNodeId", 100}, {"outPortIndex", 0}, {"inNodeId", 101}, {"inPortIndex", 0}});
        b["nodes"] = branchNodes;
        b["connections"] = branchEdges;
        auto c = graph({"NetworkInput", "Spectrogram", "NetworkOutput"},
                       {{0, {{"data_type", "mel"}, {"stream_id", "mel"}, {"provider_uuid", uuids[1]}}},
                        {1, {{"history_size", 32}}},
                        {2, {{"data_type", "history"}, {"stream_id", "history"}}}});
        QJsonObject graphs[] = {a, b, c};
        int ports[] = {ap, bp, cp};
        for (int i = 2; i >= 0; --i) {
            QJsonObject cfg{{"device", QJsonObject{{"uuid", uuids[i]}}},
                            {"core", QJsonObject{{"run_loopback", i == 1}, {"run_analyser", true}}},
                            {"aggregator", QJsonObject{{"enabled", true}, {"port", int(aggregator.serverPort())}}},
                            {"network", QJsonObject{{"stream_port", ports[i]}}},
                            {"pipeline_graph", graphs[i]}};
            if (i == 0)
                cfg["engine_server"] = QJsonObject{{"enabled", true}, {"port", control}, {"allow_mutations", true}};
            QFile f(dir.filePath(QString::number(i) + ".json"));
            require(f.open(QIODevice::WriteOnly), "write fixture");
            f.write(QJsonDocument(cfg).toJson());
            f.close();
            processes[i].setProcessChannelMode(QProcess::MergedChannels);
            processes[i].start(QCoreApplication::applicationDirPath() + "/pipeline_cli.exe",
                               {"--config", f.fileName(), "--duration", "15000"});
            require(processes[i].waitForStarted(), "CLI start");
        }
        require(wait([&] { return observer.capabilities().size() >= 3; }), "cores capabilities");
        for (auto &u : uuids) {
            auto m = observer.capabilities().value(QUuid(u));
            require(m["protocol_versions"].toArray().contains(2), "protocol capability");
        }
        auto local = graph({"NetworkInput", "Window", "WindowFunction", "StereoFFT", "BarkWeighting", "SpectrumMerge",
                            "Mel", "Spectrogram", "NetworkOutput"},
                           {{0, {{"data_type", "audio"}, {"stream_id", "local-pcm"}, {"host", "127.0.0.1"}}},
                            {7, {{"history_size", 32}}},
                            {8, {{"data_type", "history"}, {"stream_id", "local-history"}}}});
        QString error;
        auto program = GraphProgram::compile(local, error);
        require(bool(program), qPrintable(error));
        GraphEvaluationContext context;
        QMap<quint64, TypedFrame> expected, actual;
        NetworkSubscriber pcm, history, second, independent;
        int independent_count = 0;
        bool source_down = false;
        int ready = 0, second_count = 0;
        bool unavailable = false;
        auto status = [&](QString s) {
            if (s == "Running")
                ++ready;
        };
        pcm.set_status_handler(status);
        history.set_status_handler(status);
        second.set_status_handler(status);
        independent.set_status_handler(status);
        independent.set_frame_handler([&](TypedFrame f) {
            if (f.available)
                ++independent_count;
        });
        pcm.set_frame_handler([&](TypedFrame f) {
            f.producer_epoch = f.epoch;
            auto out = program->evaluate(0, f, context);
            if (out.ports.contains(8))
                expected[out.ports[8].source_sequence] = out.ports[8];
        });
        history.set_frame_handler([&](TypedFrame f) {
            if (!f.available) {
                unavailable = true;
                actual.clear();
                return;
            }
            require(!source_down || !unavailable, "stale history arrived after unavailable marker");
            actual[f.source_sequence] = std::move(f);
        });
        second.set_frame_handler([&](TypedFrame f) {
            if (f.available)
                ++second_count;
        });
        pcm.start("127.0.0.1", ap, "pcm", FrameType::PCM);
        history.start("127.0.0.1", cp, "history", FrameType::History);
        second.start("127.0.0.1", cp, "history", FrameType::History);
        independent.start("127.0.0.1", bp, "independent", FrameType::PCM);
        require(wait([&] { return ready == 4; }), "subscriptions handshake");
        // Give registry-derived bindings time to complete before starting the only source.
        QElapsedTimer settle;
        settle.start();
        wait([&] { return settle.elapsed() > 300; }, 1000);
        EngineClient client("127.0.0.1", control);
        require(wait([&] { return client.snapshot().epoch != 0; }), "control handshake");
        bool accepted = false;
        client.set_reply_handler([&](EngineReply r) {
            if (r.accepted)
                accepted = true;
        });
        EngineCommand start;
        start.kind = EngineCommand::StartCapture;
        client.submit(start);
        require(wait([&] { return accepted; }), "start capture ACK");
        require(wait([&] { return actual.size() >= 40 && expected.size() >= 40 && second_count >= 40; }, 7000),
                "three-core frames");
        int compared = 0;
        for (auto it = actual.begin(); it != actual.end(); ++it)
            if (expected.contains(it.key())) {
                compare(it.value(), expected[it.key()]);
                ++compared;
            }
        require(compared >= 30, "numeric matched sequence count");
        const auto old_epoch = client.snapshot().epoch;
        const int independent_before = independent_count;
        unavailable = false;
        source_down = true;
        client.disconnect_remote();
        processes[0].kill();
        require(processes[0].waitForFinished(2000), "publisher kill");
        require(wait([&] {
                    return observer.capabilities().value(QUuid(uuids[0]))["state"].toInt() ==
                           int(ComponentState::disconnected);
                }),
                "publisher offline registry");
        require(wait([&] { return unavailable; }), "A disconnect did not propagate unavailable through live B to C");
        require(processes[1].state() == QProcess::Running && processes[2].state() == QProcess::Running,
                "dependent cores exited");
        settle.restart();
        wait([&] { return settle.elapsed() > 300; }, 1000);
        require(independent_count > independent_before + 20, "independent B stream stopped after A disconnect");
        require(actual.isEmpty(), "history branch did not stay cleared");
        source_down = false;
        expected.clear();
        actual.clear();
        const int old_ready = ready;
        processes[0].start(QCoreApplication::applicationDirPath() + "/pipeline_cli.exe",
                           {"--config", dir.filePath("0.json"), "--duration", "15000"});
        require(processes[0].waitForStarted(), "publisher restart");
        require(wait(
                    [&] {
                        return observer.capabilities().value(QUuid(uuids[0]))["state"].toInt() >= 3 &&
                               ready > old_ready;
                    },
                    4000),
                "publisher rediscovery and subscription reconnect");
        client.connect_remote();
        require(wait([&] { return client.snapshot().epoch != 0 && client.snapshot().epoch != old_epoch; }),
                "new control epoch");
        settle.restart();
        wait([&] { return settle.elapsed() > 300; }, 1000);
        accepted = false;
        client.submit(start);
        require(wait([&] { return accepted; }), "restart capture ACK");
        require(wait([&] { return actual.size() >= 40 && expected.size() >= 40; }, 5000), "new publisher epoch frames");
        int restarted = 0;
        for (auto it = actual.begin(); it != actual.end(); ++it)
            if (expected.contains(it.key())) {
                compare(it.value(), expected[it.key()]);
                ++restarted;
            }
        require(restarted >= 30, "restart numeric comparison");
        EngineCommand stop;
        stop.kind = EngineCommand::StopCapture;
        client.submit(stop);
        require(wait([&] { return client.snapshot().statuses.value("Capture").state == State::Stopped; }),
                "capture stop");
        qInfo() << "3 CLI cores, registry UUID resolving, PCM -> FFT/Mel -> history, multicast; numeric frames"
                << compared;
        for (auto &p : processes) {
            p.terminate();
            if (!p.waitForFinished(2000)) {
                p.kill();
                p.waitForFinished();
            }
        }
        return 0;
    } catch (const std::exception &e) {
        qCritical() << e.what();
        for (auto &p : processes) {
            qCritical().noquote() << p.readAll();
            p.kill();
            p.waitForFinished(1000);
        }
        return 1;
    }
}
