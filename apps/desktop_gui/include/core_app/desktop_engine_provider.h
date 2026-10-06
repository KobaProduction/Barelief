#pragma once
#include "pipeline/engine_client.h"

// Concrete endpoint assembly belongs to desktop GUI composition. The presenter
// owns endpoints through EnginePort; this provider only retains a borrowed local
// diagnostics reference whose lifetime is bounded by the presenter.
class DesktopEngineProvider final : public pipeline::EngineProvider {
public:
    explicit DesktopEngineProvider(pipeline::AudioSourceFactory source_factory = {})
        : _source_factory(std::move(source_factory)) {}
    std::unique_ptr<pipeline::EnginePort> create(const QString &host, quint16 port) override {
        if (!host.isEmpty())
            return std::make_unique<pipeline::EngineClient>(host, port);
        auto endpoint = std::make_unique<pipeline::EngineClient>(_source_factory);
        _local_engine = endpoint->local_engine();
        return endpoint;
    }
    pipeline::AudioPipeline *local_engine() const { return _local_engine; }
private:
    pipeline::AudioSourceFactory _source_factory;
    pipeline::AudioPipeline *_local_engine = nullptr;
};
