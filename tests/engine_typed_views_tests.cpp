#include "pipeline/engine_client.h"
#include "pipeline/network_runtime.h"
#include "pipeline/stream_transport.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonArray>
#include <stdexcept>
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
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
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
            EngineClient local;
            EngineCommand command;
            command.kind = EngineCommand::ApplyGraph;
            command.graph = graph;
            local.submit(command);
            require(wait([&] { return local.snapshot().graph_revision > 0; }), "local apply");
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
            AudioPipeline engine;
            QString error;
            require(engine.apply_graph(graph, error), qPrintable(error));
            EngineServer server(engine);
            require(server.listen(QHostAddress::LocalHost, 0, true), "engine listen");
            engine.start_analyzer();
            NetworkRuntime network(engine);
            require(network.refresh(error), qPrintable(error));
            EngineClient remote("127.0.0.1", server.port());
            require(wait([&] { return remote.snapshot().epoch != 0; }), "remote handshake");
            bool received = false, cleared = false;
            remote.set_view_handler([&](ViewFrame v) {
                if (v.views.contains(1)) {
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
            engine.stop_analyzer();
            require(wait([&] { return cleared; }), "remote stopped view did not clear");
        }
        return 0;
    } catch (const std::exception &e) {
        qCritical() << e.what();
        return 1;
    }
}
