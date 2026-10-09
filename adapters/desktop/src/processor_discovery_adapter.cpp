#include "desktop/processor_discovery_adapter.h"
#include "aggregator/aggregator_client.h"

namespace desktop {
ProcessorDiscoveryAdapter::ProcessorDiscoveryAdapter(QObject *parent)
    : ProcessorDiscoveryPort(parent),
      _client(std::make_unique<AggregatorClient>(AggregatorClientConfig{QUuid::createUuid(), ComponentType::data_sink, 0})) {
    const auto notify = [this] { emit changed(); };
    connect(_client.get(), &AggregatorClient::status_changed, this, notify);
    connect(_client.get(), &AggregatorClient::capabilities_changed, this, notify);
    connect(_client.get(), &AggregatorClient::on_update_component_info, this, notify);
    connect(_client.get(), &AggregatorClient::on_update_component_state, this, notify);
}
ProcessorDiscoveryAdapter::~ProcessorDiscoveryAdapter() = default;
presentation::ProcessorDiscoverySnapshot ProcessorDiscoveryAdapter::snapshot() const {
    presentation::ProcessorDiscoverySnapshot result;
    switch (_client->runtime_state()) {
    case core::RuntimeState::Stopped: result.state = presentation::DiscoveryState::Disconnected; break;
    case core::RuntimeState::Starting: result.state = presentation::DiscoveryState::Connecting; break;
    case core::RuntimeState::Running: result.state = presentation::DiscoveryState::Connected; break;
    default: result.state = presentation::DiscoveryState::Unavailable; break;
    }
    result.error = _client->error();
    const auto components = _client->components();
    for (auto it = components.cbegin(); it != components.cend(); ++it) {
        if (it.value().type != ComponentType::core) continue;
        QString error;
        const auto metadata = _client->resolve_engine(it.key(), &error);
        presentation::ProcessorEndpoint endpoint;
        endpoint.id = it.key().toString(QUuid::WithoutBraces);
        endpoint.name = metadata.value("name").toString(endpoint.id);
        endpoint.address = metadata.value("address").toString();
        endpoint.port = quint16(metadata.value("port").toInt());
        endpoint.from_registry = true;
        endpoint.available = !metadata.isEmpty();
        endpoint.allow_mutations = metadata.value("allow_mutations").toBool();
        endpoint.error = error;
        result.processors.append(endpoint);
    }
    return result;
}
bool ProcessorDiscoveryAdapter::connect_registry(const QString &host, quint16 port) {
    const QHostAddress address(host.trimmed());
    if (address.isNull() || !port) return false;
    return _client->connect_to_aggregator_server(address, port);
}
void ProcessorDiscoveryAdapter::disconnect_registry() { _client->disconnect_from_aggregator_server(); }
}
