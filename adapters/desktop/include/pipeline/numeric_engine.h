#pragma once
#include <pipeline/engine_port.h>
#include <pipeline/audio_source.h>
#include <application/processing_control.h>
#include <QObject>
#include <memory>
#include <optional>

namespace pipeline {
// Desktop resources surround the same portable executor and application service.
// Capture callbacks process synchronously; GUI delivery contains bounded owners.
class NumericEngine final : public QObject, public EnginePort {
public:
    explicit NumericEngine(AudioSourceFactory = {}, QObject* parent=nullptr);
    ~NumericEngine() override;
    struct AdapterHandlers {
        std::function<bool(const QJsonObject&,QString*)> validate_graph;
        std::function<void(const QJsonObject&)> commit_graph;
        std::function<bool(const QJsonObject&,QString*)> configure_network;
        std::function<QString()> warning;
    };
    // Composition owns cold platform routing. Its lifetime ends before the
    // executor implementation so adapter teardown can retire subscriptions.
    void set_adapter_handlers(std::unique_ptr<QObject> owner,AdapterHandlers);
    PortDeliveryMode port_delivery_mode() const noexcept override;
    bool subscribe_numeric_frames(std::vector<EndpointIdentity>,QString* error=nullptr) override;
    void set_numeric_frame_handler(std::function<void(DeliveredNumericFrame)>) override;
    void set_numeric_history_handler(std::function<void(DeliveredNumericHistoryRow)>) override;
    quint64 submit(EngineCommand) override;
    void set_snapshot_handler(std::function<void(EngineSnapshot)>) override;
    void set_reply_handler(std::function<void(EngineReply)>) override;
    EngineSnapshot snapshot() const override;
    bool configure_network(QJsonObject,QString* error=nullptr) override;
    QString network_warning() const override;
    void connect_remote() override {}
    void disconnect_remote() override {}
    signal_contracts::Status feed_numeric(domain::NodeId,const signal_contracts::DataFrameView&);
    signal_contracts::Status feed_numeric_owned(domain::NodeId,const signal_contracts::OwnedDataFrame&,
        std::optional<std::uint64_t> history_ordinal={},std::shared_ptr<const void> retention_owner={},std::optional<std::uint64_t> history_filled_count={});
    // The callback must project borrowed service replies before returning.
    void with_control_service(const std::function<void(application::ProcessingControlService&)>&);
    // Owner-loop CONTROL reply boundary: publish committed runtime revision before the next packet.
    void synchronize_control_state(const application::ProcessingReply&);
    // Cold CONTROL owner only: register/unregister inside with_control_service.
    // Each check pins a peer reply capability snapshot through its session lifetime.
    std::uint64_t register_control_reply_check(std::function<bool(const domain::NumericProcessingGraph&)>);
    void unregister_control_reply_check(std::uint64_t);
    application::ProcessingCapabilities capabilities() const;
    // Cold per-node support query; does not prepare/apply a graph or start resources.
    domain::ProcessingDiagnostic validate_numeric_node(const domain::ConfiguredProcessingNode&) const;
    std::uint64_t subscribe_numeric_tap(std::vector<EndpointIdentity>,
        std::function<void(DeliveredNumericFrame)>,std::function<void(DeliveredNumericHistoryRow)>);
    void unsubscribe_numeric_tap(std::uint64_t);
    void update_numeric_tap(std::uint64_t,std::vector<EndpointIdentity>);
    signal_contracts::Status query_numeric(const EndpointIdentity&,DeliveredNumericFrame&,std::uint64_t* history_ordinal=nullptr,std::uint64_t* filled_count=nullptr,bool* is_history=nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AdapterHandlers adapter_handlers_;
    std::unique_ptr<QObject> adapter_owner_;
};
}
