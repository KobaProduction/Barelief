#pragma once
#include "presentation/processor_discovery_port.h"
#include <memory>
class AggregatorClient;
namespace desktop {
class ProcessorDiscoveryAdapter final : public presentation::ProcessorDiscoveryPort {
public:
    explicit ProcessorDiscoveryAdapter(QObject *parent = nullptr);
    ~ProcessorDiscoveryAdapter() override;
    presentation::ProcessorDiscoverySnapshot snapshot() const override;
    bool connect_registry(const QString &, quint16) override;
    void disconnect_registry() override;
    AggregatorClient *client() const { return _client.get(); }
private:
    std::unique_ptr<AggregatorClient> _client;
};
}
