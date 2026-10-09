#pragma once
#include "pipeline/numeric_engine.h"
#include "control/numeric_remote_endpoint.h"
#include "control/numeric_routing_adapter.h"
#include "control/numeric_desktop_host.h"
#include <aggregator/aggregator_client.h>
#include <QPointer>

// Concrete endpoint assembly belongs to desktop GUI composition. The presenter
// owns endpoints through EnginePort; this provider only retains a borrowed local
// diagnostics reference whose lifetime is bounded by the presenter.
class DesktopEngineProvider final : public pipeline::EngineProvider {
public:
    explicit DesktopEngineProvider(pipeline::AudioSourceFactory source_factory = {})
        : _source_factory(std::move(source_factory)) {}
    void set_serial_endpoint(desktop::SerialEndpoint endpoint){_serial=std::move(endpoint);}
    void set_registry(AggregatorClient* registry) {
        _registry=registry;
        bind_registry();
    }
    std::unique_ptr<pipeline::EnginePort> create(const QString &host, quint16 port) override {
        auto endpoint=std::make_unique<pipeline::NumericDesktopHost>(_source_factory,host,port,
            _serial && host==_serial->port_name?_serial:std::nullopt);
        if(host.isEmpty()){_local_host=endpoint.get();_local_engine=endpoint->local_engine();bind_registry();}
        return endpoint;
    }
    pipeline::NumericEngine *local_engine() const { return _local_engine; }
private:
    pipeline::AudioSourceFactory _source_factory;
    pipeline::NumericEngine *_local_engine = nullptr;
    std::optional<desktop::SerialEndpoint> _serial;
    QPointer<AggregatorClient> _registry;
    QPointer<pipeline::NumericDesktopHost> _local_host;
    QMetaObject::Connection _connect_registry,_disconnect_registry;
    void bind_registry(){
        QObject::disconnect(_connect_registry);QObject::disconnect(_disconnect_registry);
        if(!_local_host)return;
        if(!_registry){_local_host->disconnect_registry();return;}
        auto* host=_local_host.data();
        _connect_registry=QObject::connect(_registry,&AggregatorClient::connection_requested,host,[host](QHostAddress address,quint16 port,qint32 timeout){host->configure_registry(address,port,timeout);});
        _disconnect_registry=QObject::connect(_registry,&AggregatorClient::disconnection_requested,host,[host]{host->disconnect_registry();});
        if(const auto request=_registry->connection_request())host->configure_registry(request->address,request->port,request->timeout_ms);
    }
};
