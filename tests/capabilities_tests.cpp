#include "aggregator/aggregator_client.h"
#include "aggregator/aggregator_server.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonArray>
#include <stdexcept>
static void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
static bool wait(std::function<bool()> f) {
    QElapsedTimer t;
    t.start();
    while (!f() && t.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return f();
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        AggregatorServer s;
        require(s.start(0), "start");
        QUuid uuid = QUuid::createUuid();
        AggregatorClient a({uuid, ComponentType::core, 8213, 321});
        a.set_capabilities({{"stream_port", 8214},
                            {"streams", QJsonArray{QJsonObject{{"id", "mel"}, {"data_type", "mel"}}}},
                            {"node_kinds", QJsonArray{"Mel"}},
                            {"allow_mutations", false}});
        a.connect_to_aggregator_server(QHostAddress::LocalHost, s.serverPort(), 2000);
        require(wait([&] { return a.is_registered(); }), "registration");
        AggregatorClient duplicate({uuid, ComponentType::core, 8213, 321});
        duplicate.set_reconnect_mode(false, 0);
        duplicate.connect_to_aggregator_server(QHostAddress::LocalHost, s.serverPort(), 2000);
        require(wait([&] { return duplicate.runtime_state() == core::RuntimeState::Failed; }),
                "live UUID with matching session token was not rejected");
        require(a.is_registered() && a.is_connected(), "duplicate displaced active provider");
        AggregatorClient b({QUuid::createUuid(), ComponentType::data_sink, 0});
        b.connect_to_aggregator_server(QHostAddress::LocalHost, s.serverPort(), 2000);
        require(wait([&] { return b.capabilities().contains(uuid); }), "metadata snapshot");
        auto m = b.capabilities()[uuid];
        require(m["stream_port"] == 8214 && m["node_kinds"].toArray().contains("Mel") && !m["allow_mutations"].toBool(),
                "capability fields");
        a.disconnect_from_aggregator_server();
        require(
            wait([&] { return b.capabilities().value(uuid)["state"].toInt() == int(ComponentState::disconnected); }),
            "availability");
        a.connect_to_aggregator_server(QHostAddress::LocalHost, s.serverPort(), 2000);
        require(wait([&] { return b.capabilities().value(uuid)["state"].toInt() >= 3; }), "restore");
        return 0;
    } catch (const std::exception &e) {
        qCritical() << e.what();
        return 1;
    }
}
