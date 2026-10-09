#pragma once
#include <QObject>
#include <QJsonObject>
#include <memory>
class AggregatorClient;
namespace pipeline {
class NumericEngine;
// Qt control-plane composition. Engine and optional registry outlive this owner.
// Routes never run DSP or retain borrowed frame views. Calls use the Qt thread.
class NumericRoutingAdapter final : public QObject {
public:
    static constexpr std::size_t MaxRoutes=128,MaxPeers=32,MaxHistoryEndpointsPerPeer=8;
    NumericRoutingAdapter(NumericEngine&,AggregatorClient* registry=nullptr,QObject* parent=nullptr);
    ~NumericRoutingAdapter() override;
    void set_registry(AggregatorClient*);
    bool validate_graph_routes(const QJsonObject&,QString* error=nullptr) const;
    // network.routes: [{node_id, provider_uuid, stream_id}] or
    // [{node_id,host,port,remote_endpoint:{graph_id,node_id,direction,port_index}}].
    // Directory routes require canonical CONTROL2/DATA4 endpoint metadata.
    bool configure(const QJsonObject& runtime_config,QString* error=nullptr);
    // Supply ORIGINAL metadata only AFTER a successful graph Apply. Explicit
    // network.routes take precedence over graph nodes' adapter metadata. Visual3
    // internal-data adapter keys are supported; no legacy graph is executed.
    bool configure_graph_routes(const QJsonObject& accepted_graph,QString* error=nullptr);
    bool refresh(QString* error=nullptr);
    QJsonObject diagnostics() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
