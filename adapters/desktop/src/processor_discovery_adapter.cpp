#include "desktop/processor_discovery_adapter.h"
#include "aggregator/aggregator_client.h"

namespace desktop {
namespace {
presentation::DiscoveryState discovery_state(core::RuntimeState state) {
    switch(state) {
    case core::RuntimeState::Stopped:return presentation::DiscoveryState::Disconnected;
    case core::RuntimeState::Starting:return presentation::DiscoveryState::Connecting;
    case core::RuntimeState::Running:return presentation::DiscoveryState::Connected;
    default:return presentation::DiscoveryState::Unavailable;
    }
}

presentation::ProcessorEndpoint processor_endpoint(const AggregatorClient& client,const QUuid& id) {
    QString error;
    const auto metadata=client.resolve_engine(id,&error);
    presentation::ProcessorEndpoint endpoint;
    endpoint.id=id.toString(QUuid::WithoutBraces);
    endpoint.name=metadata.value("name").toString(endpoint.id);
    endpoint.address=metadata.value("address").toString();
    endpoint.port=quint16(metadata.value("port").toInt());
    endpoint.from_registry=true;
    endpoint.available=!metadata.isEmpty();
    endpoint.allow_mutations=metadata.value("allow_mutations").toBool();
    endpoint.error=error;
    return endpoint;
}
}
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
    result.state=discovery_state(_client->runtime_state());
    result.error = _client->error();
    const auto components = _client->components();
    for (auto it = components.cbegin(); it != components.cend(); ++it) {
        if (it.value().type != ComponentType::core) continue;
        result.processors.append(processor_endpoint(*_client,it.key()));
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
