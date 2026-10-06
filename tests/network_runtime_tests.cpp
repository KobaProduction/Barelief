#include "synthetic_capture.h"
#include "pipeline/engine_client.h"
#include "pipeline/stream_transport.h"
#include "pipeline/network_runtime.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTcpServer>
#include <QJsonArray>
#include <atomic>
#include <thread>
#include <stdexcept>
using namespace pipeline;
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
static bool wait(std::function<bool()> f) {
    QElapsedTimer t;
    t.start();
    while (!f() && t.elapsed() < 2500) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return f();
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if(app.arguments().contains("--network-apply-only")) {
            NetworkPublisher publisher;publisher.advertise("mel",FrameType::Mel);
            require(publisher.listen(),"apply-only publisher");
            AudioPipeline engine(test_fixture::synthetic_capture_factory());QString error;
            const auto graph=QJsonObject{{"version",2},{"nodes",QJsonArray{
                QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","mel"},
                    {"stream_id","mel"},{"host","127.0.0.1"},{"port",publisher.port()}}}},
                QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","MelView"}}}}}},
                {"connections",QJsonArray{QJsonObject{{"outNodeId",0},{"inNodeId",1},{"outPortIndex",0},{"inPortIndex",0}}}}};
            require(engine.apply_graph(graph,error),qPrintable(error));
            NetworkRuntime runtime(engine);require(runtime.refresh(error),qPrintable(error));
            TypedFrame mel;mel.type=FrameType::Mel;mel.sample_rate=48000;mel.values={1,2,3};mel.stream_id="mel";
            require(wait([&] {
                ++mel.sequence;publisher.publish("mel",mel);const auto views=engine.latest_views();
                return views.contains(1) && views[1].available && views[1].values==mel.values;
            }),
                    "Initial NetworkInput Apply did not start application processing");
            engine.stop_analyzer();
            auto changed=graph;changed["graph_id"]="stopped";
            require(engine.apply_graph(changed,error),qPrintable(error));
            require(engine.current_status("Analyzer").state==State::Stopped,"Network Apply lost explicit Stop intent");
            return 0;
        }
        QTcpServer registry_probe;
        require(registry_probe.listen(QHostAddress::LocalHost, 0), "registry probe");
        QTcpServer reserve;
        require(reserve.listen(QHostAddress::LocalHost, 0), "port probe");
        int port = reserve.serverPort();
        EngineClient local(test_fixture::synthetic_capture_factory());
        QString error;
        require(local.configure_network(
                    {{"aggregator", QJsonObject{{"enabled", true}, {"port", int(registry_probe.serverPort())}}},
                     {"network", QJsonObject{{"stream_port", port}}}},
                    &error),
                "local configuration opened external service");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        QCoreApplication::processEvents();
        require(!registry_probe.hasPendingConnections(), "local graph connected registry");
        reserve.close();
        auto graph = GraphProgram::test_signal_defaults();
        auto nodes = graph["nodes"].toArray();
        QJsonArray sourceOnly;
        sourceOnly.append(nodes[0]);
        sourceOnly.append(
            QJsonObject{{"id", 100},
                        {"internal-data",
                         QJsonObject{{"model-name", "NetworkOutput"}, {"data_type", "audio"}, {"stream_id", "pcm"}}}});
        graph["version"] = 2;
        graph["nodes"] = sourceOnly;
        graph["connections"] =
            QJsonArray{QJsonObject{{"outNodeId", 0}, {"outPortIndex", 0}, {"inNodeId", 100}, {"inPortIndex", 0}}};
        EngineCommand c;
        c.kind = EngineCommand::ApplyGraph;
        c.graph = graph;
        bool applied = false;
        local.set_reply_handler([&](EngineReply r) {
            if (r.accepted)
                applied = true;
        });
        local.submit(c);
        require(wait([&] { return applied; }), "apply source graph");
        require(local.configure_network({{"network", QJsonObject{{"stream_port", port}}}}, &error), qPrintable(error));
        QThread receive;
        QObject worker;
        worker.moveToThread(&receive);
        receive.start();
        NetworkSubscriber *subscriber = nullptr;
        std::atomic<int> count = 0;
        std::atomic<bool> ready = false;
        QMetaObject::invokeMethod(
            &worker,
            [&] {
                subscriber = new NetworkSubscriber;
                subscriber->set_status_handler([&](QString s) {
                    if (s == "Running")
                        ready = true;
                });
                subscriber->set_frame_handler([&](TypedFrame f) {
                    require(f.type == FrameType::PCM, "source did not emit PCM");
                    ++count;
                });
                subscriber->start("127.0.0.1", port, "pcm", FrameType::PCM);
            },
            Qt::BlockingQueuedConnection);
        c.kind = EngineCommand::StartAnalyzer;
        local.submit(c);
        require(wait([&] { return ready.load(); }), "subscriber ready");
        c.kind = EngineCommand::StartCapture;
        local.submit(c);
        require(wait([&] { return count > 10; }), "PCM ingress");
        const int before = count.load();
        // Main/UI event loop is intentionally blocked. Transport owns its own event loop.
        std::this_thread::sleep_for(std::chrono::seconds(5));
        require(count.load() > before + 500, "blocked UI stalled network runtime");
        c.kind = EngineCommand::StopCapture;
        local.submit(c);
        wait([&] { return local.snapshot().statuses.value("Capture").state == State::Stopped; });
        c.kind = EngineCommand::StopAnalyzer;
        local.submit(c);
        require(wait([&] { return local.snapshot().statuses.value("Analyzer").state == State::Stopped; }),
                "stop analyzer");
        const int stopped = count.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(count.load() == stopped, "stopped runtime delivered old frame");
        c.kind = EngineCommand::StartAnalyzer;
        local.submit(c);
        c.kind = EngineCommand::StartCapture;
        local.submit(c);
        require(wait([&] { return count.load() > stopped + 10; }), "start did not rebind new receiver epoch");
        c.kind = EngineCommand::StopCapture;
        local.submit(c);
        require(wait([&] { return local.snapshot().statuses.value("Capture").state == State::Stopped; }),
                "final capture stop");
        QMetaObject::invokeMethod(&worker, [&] { delete subscriber; }, Qt::BlockingQueuedConnection);
        receive.quit();
        receive.wait();
        return 0;
    } catch (const std::exception &e) {
        qCritical() << e.what();
        return 1;
    }
}
