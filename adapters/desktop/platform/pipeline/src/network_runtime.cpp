#include "pipeline/network_runtime.h"
#include "pipeline/stream_transport.h"
#include "pipeline/serial_config.h"
#include "aggregator/aggregator_client.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QThread>
#include <QRandomGenerator>
#include <deque>
#include <mutex>
namespace pipeline {
namespace {
FrameType type(QString s) {
    return s == "audio"             ? FrameType::PCM
           : s == "window"          ? FrameType::Window
           : s == "windowed"        ? FrameType::Windowed
           : s == "stereo-spectrum" ? FrameType::StereoSpectrum
           : s == "spectrum"        ? FrameType::Spectrum
           : s == "history"         ? FrameType::History
           : s == "scalar"          ? FrameType::Scalar
           : s == "wled-packet"     ? FrameType::WledPacket
                                    : FrameType::Mel;
}
QString data_type(QJsonObject p) {
    return p.value("data_type").toString(p.value("type").toString("mel"));
}
} // namespace
class NetworkState {
  public:
    AudioPipeline &engine;
    QObject *owner;
    QJsonObject config;
    QTimer poll;
    uint64_t token = 0, revision = UINT64_MAX, receiver_epoch = UINT64_MAX;
    bool running = false;
    SerialBinding serial;
    std::unique_ptr<NetworkPublisher> publisher;
    std::unique_ptr<AggregatorClient> registry;
    struct Input {
        int node;
        QJsonObject params;
        std::unique_ptr<NetworkSubscriber> subscriber;
        QString endpoint;
    };
    std::vector<Input> inputs;
    QMap<int, QString> outputs;
    std::mutex mutex;
    std::deque<std::pair<int, DeliveredPortFrame>> queued;
    size_t queued_bytes = 0;
    uint64_t dropped_frames = 0;
    QMap<int, bool> gaps;
    QMap<int, DeliveredPortFrame> unavailable;
    QJsonObject metadata;
    NetworkState(AudioPipeline &e, QJsonObject c, QObject *o) : engine(e), owner(o), config(std::move(c)), poll(o) {
        poll.setInterval(5);
        QObject::connect(&poll, &QTimer::timeout, o, [this] {
            if (revision != engine.graph_revision() || receiver_epoch != engine.epoch() ||
                running != (engine.current_status("Analyzer").state == State::Running)) {
                QString error;
                if (!refresh(error))
                    qWarning().noquote() << "Network:" << error;
            }
            std::deque<std::pair<int, DeliveredPortFrame>> batch;
            QMap<int, DeliveredPortFrame> controls;
            {
                std::lock_guard lock(mutex);
                queued.swap(batch);
                controls.swap(unavailable);
                queued_bytes = 0;
            }
            for (auto it = controls.begin(); it != controls.end(); ++it)
                if (publisher && outputs.contains(it.key()) && it->delivery.epoch == engine.epoch() &&
                    it->delivery.graph_revision == engine.graph_revision())
                    if(!publisher->publish(outputs[it.key()], it->frame))qWarning()<<"NetworkOutput delivery refused"<<it.key();
            for (auto &[node, f] : batch)
                if (publisher && outputs.contains(node) && f.delivery.graph_revision == engine.graph_revision() &&
                    f.delivery.epoch == engine.epoch())
                    if(!publisher->publish(outputs[node], f.frame))qWarning()<<"NetworkOutput delivery refused"<<node;
        });
        poll.start();
    }
    void stop() {
        if (token) {
            engine.unsubscribe_ports(token);
            token = 0;
        }
        inputs.clear();
        publisher.reset();
        registry.reset();
        outputs.clear();
        std::lock_guard lock(mutex);
        queued.clear();
        queued_bytes = 0;
        gaps.clear();
        unavailable.clear();
    }
    void resolve(Input &i) {
        auto p = i.params;
        if(serial.configured && serial.input_node==i.node) {
            const auto endpoint=serial.endpoint.port_name+":"+QString::number(serial.endpoint.baud_rate);
            if(endpoint==i.endpoint) return;
            i.endpoint=endpoint;
            i.subscriber->start_serial(serial.endpoint,p.value("stream_id").toString(),type(data_type(p)));
            return;
        }
        QString host = p.value("host").toString();
        int port = p.value("port").toInt(8214);
        const QUuid provider(p.value("provider_uuid").toString());
        QString incarnation,binding_identity;
        uint64_t expected_incarnation=0;
        StreamLimits limits;
        if (!provider.isNull() && registry) {
            auto m = registry->capabilities().value(provider);
            if (m.value("state").toInt() < 3) {
                i.subscriber->stop();
                i.endpoint.clear();
                return;
            }
            host = m.value("address").toString();
            port = m.value("stream_port").toInt();
            if(int(type(data_type(p)))>6 && !m.value("stream_delivery_features").toArray().contains("port-identity-v1")) {
                i.subscriber->stop();i.endpoint.clear();qWarning()<<"NetworkInput"<<i.node<<"provider does not explicitly support Scalar/Packet";return;
            }
            const auto streams=m.value("streams").toArray();
            const auto selected=std::find_if(streams.begin(),streams.end(),[&](const auto &value){const auto s=value.toObject();return s.value("id").toString()==p.value("stream_id").toString() && s.value("data_type").toString()==data_type(p);});
            if(selected==streams.end()){i.subscriber->stop();i.endpoint.clear();return;}
            incarnation=m.value("publisher_incarnation").toString();
            if(m.value("stream_delivery_features").toArray().contains("port-identity-v1")) {
                bool valid_incarnation=false;expected_incarnation=incarnation.toULongLong(&valid_incarnation);
                if(!valid_incarnation || !expected_incarnation || QString::number(expected_incarnation)!=incarnation){i.subscriber->stop();i.endpoint.clear();return;}
                const auto stream_binding=selected->toObject();QJsonObject identity;
                for(const auto key:{"graph_id","node_id","direction","port_index","id","data_type","payload_version","subscription_version"})identity[key]=stream_binding.value(key);
                binding_identity=QString::fromUtf8(QJsonDocument(identity).toJson(QJsonDocument::Compact));
                const auto declared=m.value("stream_limits").toObject();
                limits={size_t(declared.value("max_frame_bytes").toInt()),size_t(declared.value("max_stream_id_bytes").toInt()),size_t(declared.value("max_streams").toInt())};
            }
        }
        if (host.isEmpty() || port <= 0 || port > 65535)
            return;
        QString endpoint = host + ":" + QString::number(port)+":"+incarnation+":"+QString::number(limits.max_frame_bytes)+":"+QString::number(limits.max_stream_id_bytes)+":"+QString::number(limits.max_streams)+":"+binding_identity;
        if (endpoint == i.endpoint)
            return;
        i.endpoint = endpoint;
        i.subscriber->start(host, port, p.value("stream_id").toString("default"), type(data_type(p)),limits,expected_incarnation);
    }
    bool refresh(QString &error) {
        stop();
        revision = engine.graph_revision();
        receiver_epoch = engine.epoch();
        running = engine.current_status("Analyzer").state == State::Running;
        auto graph = engine.graph();
        QMap<int, QJsonObject> nodes;
        for (auto v : graph["nodes"].toArray()) {
            auto n = v.toObject();
            nodes[n["id"].toInt()] = n["internal-data"].toObject();
        }
        serial=read_serial_binding(config);
        if(!serial) {error=serial.error;return false;}
        if(serial.configured && nodes.value(serial.input_node).value("model-name")!="NetworkInput") {
            error="network.serial input_node must identify an existing NetworkInput";return false;
        }
        QSet<int> active;
        for (auto v : graph["connections"].toArray()) {
            auto c = v.toObject();
            active.insert(c["outNodeId"].toInt());
            active.insert(c["inNodeId"].toInt());
        }
        QSet<int> reachable;
        for (auto it = nodes.begin(); it != nodes.end(); ++it)
            if (node_definition(it.value()["model-name"].toString()).input.isEmpty())
                reachable.insert(it.key());
        for (int pass = 0; pass < nodes.size(); ++pass)
            for (auto value : graph["connections"].toArray()) {
                const auto edge = value.toObject();
                if (reachable.contains(edge["outNodeId"].toInt()))
                    reachable.insert(edge["inNodeId"].toInt());
            }
        active.intersect(reachable);
        auto network = config.value("network").toObject();
        QJsonArray streams;
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            auto p = it.value();
            auto kind = p["model-name"].toString();
            if (!active.contains(it.key()) || !running)
                continue;
            if (kind == "NetworkOutput") {
                if (p.value("stream_id").toString().isEmpty()) {
                    error = "NetworkOutput requires stream_id";
                    stop();
                    return false;
                }
                if (!publisher)
                    publisher = std::make_unique<NetworkPublisher>(owner);
                auto id = p.value("stream_id").toString("default");
                if (outputs.values().contains(id)) {
                    error = "Duplicate output stream_id";
                    stop();
                    return false;
                }
                outputs[it.key()] = id;
                if(!publisher->try_advertise(id,type(data_type(p)))){error="Output exceeds stream limits or changes stream type";stop();return false;}
                const int version=int(type(data_type(p)))>6?3:2;
                streams.append(QJsonObject{{"id", id}, {"data_type", data_type(p)},
                    {"graph_id",graph.value("graph_id").toString("main")},{"node_id",it.key()},
                    {"direction","input"},{"port_index",0},{"payload_version",version},{"subscription_version",version}});
            }
            if (kind == "NetworkInput") {
                const bool uart=serial.configured && serial.input_node==it.key();
                if (p.value("stream_id").toString().isEmpty() ||
                    (!uart && p.value("host").toString().isEmpty() && QUuid(p.value("provider_uuid").toString()).isNull())) {
                    error = "NetworkInput requires stream_id and provider_uuid or host";
                    stop();
                    return false;
                }
                Input i;
                i.node = it.key();
                i.params = p;
                i.subscriber = std::make_unique<NetworkSubscriber>(owner);
                int node = i.node;
                auto receiver_epoch = engine.epoch();
                i.subscriber->set_frame_handler([this, node, receiver_epoch](TypedFrame f) {
                    const auto serial = engine.ingress_serial(node);
                    engine.feed_typed(node, std::move(f), receiver_epoch, serial);
                });
                i.subscriber->set_status_handler([this, node, receiver_epoch](QString s) {
                    if (s != "Running")
                        engine.clear_ingress(node, receiver_epoch);
                    qInfo() << "NetworkInput" << node << s;
                });
                inputs.push_back(std::move(i));
            }
        }
        if (publisher) {
            QHostAddress address(network.value("listen_address").toString("127.0.0.1"));
            int port = network.value("stream_port").toInt(8214);
            if (address.isNull() || port < 0 || port > 65535 || !publisher->listen(address, port)) {
                error = "Cannot listen on typed stream endpoint";
                stop();
                return false;
            }
        }
        QJsonArray kinds;
        for (auto &def : node_definitions())
            kinds.append(def.name);
        metadata = {
            {"version", 2},
            {"graph_version", 2},
            {"processing_version",1},
            {"protocol_versions", QJsonArray{1, 2, 3}},
            {"node_kinds", kinds},
            {"allow_mutations", config.value("engine_server").toObject().value("allow_mutations").toBool(false)},
            {"engine_port", config.value("engine_server").toObject().value("enabled").toBool(false)
                                ? config.value("engine_server").toObject().value("port").toInt(8213)
                                : 0},
            {"control_available", config.value("engine_server").toObject().value("enabled").toBool(false)},
            {"stream_port", publisher ? publisher->port() : 0},
            {"streams", streams}};
        metadata["stream_delivery_features"]=QJsonArray{"port-identity-v1"};
        metadata["stream_protocol_versions"]=QJsonArray{2,3};
        const auto stream_limits=publisher?publisher->limits():StreamLimits{};
        metadata["stream_limits"]=QJsonObject{{"max_frame_bytes",int(stream_limits.max_frame_bytes)},{"max_stream_id_bytes",int(stream_limits.max_stream_id_bytes)},{"max_streams",int(stream_limits.max_streams)}};
        metadata["publisher_incarnation"]=QString::number(publisher?publisher->incarnation():uint64_t(1));
        const auto aggregate = config.value("aggregator").toObject();
        if ((publisher || !inputs.empty() || config.value("engine_server").toObject().value("enabled").toBool()) &&
            aggregate.value("enabled").toBool(false)) {
            QUuid uuid(config.value("device").toObject().value("uuid").toString());
            if (uuid.isNull()) {
                error = "Aggregator requires stable device.uuid";
                stop();
                return false;
            }
            const QHostAddress registry_address(aggregate.value("host").toString("127.0.0.1"));
            const int registry_port = aggregate.value("port").toInt(8212);
            const int session_token =
                aggregate.value("session_token").toInt(qChecksum(QByteArrayView(uuid.toRfc4122())));
            if (registry_address.isNull() || registry_port < 1 || registry_port > 65535 || session_token < 0 ||
                session_token > 65535) {
                error = "Invalid aggregator address, port or session token";
                stop();
                return false;
            }
            registry = std::make_unique<AggregatorClient>(
                AggregatorClientConfig{uuid, ComponentType::core, quint16(metadata.value("engine_port").toInt()),
                                       quint16(session_token)},
                owner);
            registry->set_capabilities(metadata);
            QObject::connect(registry.get(), &AggregatorClient::capabilities_changed, owner, [this](auto, auto) {
                for (auto &i : inputs)
                    resolve(i);
            });
            registry->set_reconnect_mode(true);
            QObject::connect(registry.get(), &AggregatorClient::status_changed, owner,
                             [](auto state, const QString &error) {
                                 if (!error.isEmpty())
                                     qWarning() << "Registry" << int(state) << error;
                             });
            registry->connect_to_aggregator_server(registry_address, quint16(registry_port));
        }
        for (auto &i : inputs)
            resolve(i);
        if (publisher) {
            std::vector<EndpointIdentity> endpoints;
            for(auto node:outputs.keys())endpoints.push_back({graph.value("graph_id").toString("main").toStdString(),node,PortDirection::Input,0});
            token = engine.subscribe_ports([this, binding_epoch = receiver_epoch,
                                            binding_revision = revision](DeliveredPortFrame delivered) {
                const auto node=delivered.endpoint.node;
                auto &f=delivered.frame;
                if (delivered.endpoint.direction!=PortDirection::Input || delivered.endpoint.port!=0 || !outputs.contains(node) ||
                    delivered.delivery.epoch != binding_epoch || delivered.delivery.graph_revision != binding_revision)
                    return;
                f.stream_id = outputs[node].toStdString();
                if (int(f.type) <= 3) {
                    f.values.clear();
                    f.history.clear();
                } else if (f.type == FrameType::History) {
                    f.left.clear();
                    f.right.clear();
                    f.values.clear();
                } else {
                    f.left.clear();
                    f.right.clear();
                    f.history.clear();
                }
                const auto bytes = stream::payload_size(f);
                std::lock_guard lock(mutex);
                if (!f.available) {
                    for (auto it = queued.begin(); it != queued.end();) {
                        if (it->first == node) {
                            queued_bytes -= stream::payload_size(it->second.frame);
                            it = queued.erase(it);
                        } else
                            ++it;
                    }
                    f.stream_id = outputs[node].toStdString();
                    unavailable[node] = std::move(delivered);
                    gaps[node] = true;
                    return;
                }
                if (bytes > stream::MaxFrameBytes || queued_bytes + bytes > 4 * 1024 * 1024 || queued.size() >= 64) {
                    gaps[node] = true;
                    ++dropped_frames;
                    return;
                }
                f.stream_id = outputs[node].toStdString();
                f.discontinuity |= gaps.take(node);
                queued_bytes += bytes;
                queued.emplace_back(node, std::move(delivered));
            },std::move(endpoints));
            if(!token){error="Cannot subscribe exact NetworkOutput sink endpoints";stop();return false;}
        }
        error.clear();
        return true;
    }
    ~NetworkState() {
        poll.stop();
        stop();
    }
};
class NetworkRuntime::Impl {
  public:
    QThread thread;
    QObject *worker = nullptr;
    NetworkState *state = nullptr;
    Impl(AudioPipeline &e, QJsonObject c) {
        worker = new QObject;
        worker->moveToThread(&thread);
        thread.start();
        QMetaObject::invokeMethod(
            worker, [&] { state = new NetworkState(e, std::move(c), worker); }, Qt::BlockingQueuedConnection);
    }
    ~Impl() {
        QMetaObject::invokeMethod(
            worker,
            [this] {
                delete state;
                state = nullptr;
            },
            Qt::BlockingQueuedConnection);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.quit();
        thread.wait();
    }
};
NetworkRuntime::NetworkRuntime(AudioPipeline &e, QJsonObject c, QObject *p)
    : QObject(p), d(std::make_unique<Impl>(e, std::move(c))) {}
NetworkRuntime::~NetworkRuntime() = default;
bool NetworkRuntime::refresh(QString &e) {
    bool result = false;
    QMetaObject::invokeMethod(d->worker, [&] { result = d->state->refresh(e); }, Qt::BlockingQueuedConnection);
    return result;
}
QJsonObject NetworkRuntime::capabilities() const {
    QJsonObject result;
    QMetaObject::invokeMethod(d->worker, [&] { result = d->state->metadata; }, Qt::BlockingQueuedConnection);
    return result;
}
QJsonObject NetworkRuntime::diagnostics() const {
    QJsonObject result;
    QMetaObject::invokeMethod(
        d->worker,
        [&] {
            std::lock_guard lock(d->state->mutex);
            result = {{"mailbox_bytes", qint64(d->state->queued_bytes)},
                      {"mailbox_frames", int(d->state->queued.size())},
                      {"mailbox_dropped", qint64(d->state->dropped_frames)},
                      {"subscriber_dropped", qint64(d->state->publisher ? d->state->publisher->dropped() : 0)},
                      {"receiver_epoch", QString::number(d->state->receiver_epoch)}};
        },
        Qt::BlockingQueuedConnection);
    return result;
}
} // namespace pipeline
