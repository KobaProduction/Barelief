#include "aggregator/aggregator_client.h"
#include "aggregator/aggregator_server.h"
#include "pipeline/engine_client.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTcpServer>
#include <stdexcept>
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
static bool wait(std::function<bool()> f) {
    QElapsedTimer t;
    t.start();
    while (!f() && t.elapsed() < 1600) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return f();
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        AggregatorServer directory;
        require(directory.start(0), "registry fixture listen");
        const auto registry_port = directory.serverPort();
        pipeline::AudioPipeline engine;
        engine.stop_capture();
        engine.stop_analyzer();
        {
            pipeline::EngineClient local;
            QJsonObject config{{"engine_server", QJsonObject{{"enabled", true}, {"port", 8213}}}};
            QString error;
            require(local.configure_network(config, &error), "local network configuration");
            require(!local.network_warning().isEmpty(), "GUI executor hides unavailable control endpoint");
            require(config["engine_server"].toObject()["enabled"].toBool(), "composition changed caller document");
        }
        auto endpoint = std::make_unique<pipeline::EngineServer>(engine);
        require(endpoint->listen(QHostAddress::LocalHost, 0), "core fixture listen");
        const auto engine_port = endpoint->port();
        const auto id = QUuid::createUuid();
        AggregatorClient core({id, ComponentType::core, engine_port, 321});
        AggregatorClient observer({QUuid::createUuid(), ComponentType::data_sink, 0});
        core.set_reconnect_mode(true, 3, 30);
        observer.set_reconnect_mode(true, 3, 30);
        core.connect_to_aggregator_server(QHostAddress::LocalHost, registry_port, 1000);
        observer.connect_to_aggregator_server(QHostAddress::LocalHost, registry_port, 1000);
        QString error;
        require(wait([&] { return !observer.resolve_engine(id, &error).isEmpty(); }),
                "registered control endpoint not discoverable");
        auto discovered = observer.resolve_engine(id, &error);
        require(discovered["port"].toInt() == engine_port, "registry confused stream/control port");
        pipeline::EngineClient remote(discovered["address"].toString(), discovered["port"].toInt());
        require(wait([&] { return remote.snapshot().epoch != 0; }), "discovered core cannot be opened");
        const auto previous_epoch = remote.snapshot().epoch;
        core.disconnect_from_aggregator_server();
        endpoint.reset();
        require(wait([&] { return observer.resolve_engine(id, &error).isEmpty() && remote.snapshot().epoch == 0; }),
                "offline core remains selectable/connected");
        require(error.contains("unavailable"), "offline core lacks actionable error");
        endpoint = std::make_unique<pipeline::EngineServer>(engine);
        require(endpoint->listen(QHostAddress::LocalHost, engine_port), "core return listen");
        core.connect_to_aggregator_server(QHostAddress::LocalHost, registry_port, 1000);
        require(wait([&] {
                    return !observer.resolve_engine(id, &error).isEmpty() && remote.snapshot().epoch != 0 &&
                           remote.snapshot().epoch != previous_epoch;
                }),
                "online return did not rediscover/reconnect");
        directory.stop();
        require(wait([&] { return !observer.is_registered(); }), "registry loss ignored");
        require(observer.resolve_engine(id, &error).isEmpty(), "registry loss retained stale online endpoint");
        require(!observer.capabilities().value(id)["registry_available"].toBool(true),
                "cached metadata still claims directory availability");
        require(directory.start(registry_port), "registry return listen");
        require(wait([&] { return !observer.resolve_engine(id, &error).isEmpty(); }),
                "registry return did not refresh v1 component availability");
        const auto stream_id = QUuid::createUuid();
        AggregatorClient stream_only({stream_id, ComponentType::core, 0});
        stream_only.connect_to_aggregator_server(QHostAddress::LocalHost, registry_port, 1000);
        require(wait([&] { return stream_only.is_registered() && observer.components().contains(stream_id); }),
                "stream-only fixture registration");
        require(observer.resolve_engine(stream_id, &error).isEmpty() && error.contains("no control endpoint"),
                "stream-only core advertised a fake editor endpoint");
        QTcpServer probe;
        require(probe.listen(QHostAddress::LocalHost, 0), "unused registry port");
        const auto absent_port = probe.serverPort();
        probe.close();
        AggregatorClient absent({QUuid::createUuid(), ComponentType::data_sink, 0});
        absent.set_reconnect_mode(false, 0);
        absent.connect_to_aggregator_server(QHostAddress::LocalHost, absent_port, 100);
        require(wait([&] { return absent.runtime_state() == core::RuntimeState::Failed; }),
                "missing registry remained ambiguous");
        require(absent.discovery_status().contains("Start or configure an aggregator") &&
                    absent.discovery_status().contains(QString::number(absent_port)),
                "missing registry error has no action/address");
        return 0;
    } catch (const std::exception &e) {
        qCritical() << e.what();
        return 1;
    }
}
