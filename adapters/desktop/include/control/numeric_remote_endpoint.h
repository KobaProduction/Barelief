#pragma once
#include <pipeline/engine_port.h>
#include <desktop/qt_byte_transport.h>
#include <QObject>
namespace pipeline {
class NumericRemoteEndpoint final : public QObject, public EnginePort {
public:
    NumericRemoteEndpoint(QString host,quint16 port,QObject* parent=nullptr);
    explicit NumericRemoteEndpoint(desktop::SerialEndpoint,QObject* parent=nullptr);
    ~NumericRemoteEndpoint() override;
    PortDeliveryMode port_delivery_mode() const noexcept override;
    bool subscribe_numeric_frames(std::vector<EndpointIdentity>,QString* error=nullptr) override;
    void set_numeric_frame_handler(std::function<void(DeliveredNumericFrame)>) override;
    void set_numeric_history_handler(std::function<void(DeliveredNumericHistoryRow)>) override;
    // Cold full History query, retaining the exact DATA4 accepted-row ordinal.
    // Invoked outside parser callbacks; owned arena remains pinned by context.
    void set_numeric_history_snapshot_handler(std::function<void(DeliveredNumericFrame,std::uint64_t,std::uint64_t)>) override;
    quint64 submit(EngineCommand) override;
    void set_snapshot_handler(std::function<void(EngineSnapshot)>) override;
    void set_reply_handler(std::function<void(EngineReply)>) override;
    EngineSnapshot snapshot() const override;
    bool configure_network(QJsonObject,QString* error=nullptr) override;
    QString network_warning() const override;
    void connect_remote() override;
    void disconnect_remote() override;
    // Explicit ingress identity; retains a bounded owner, never a borrowed view.
    bool publish_numeric_frame(const EndpointIdentity&,DeliveredNumericFrame,QString* error=nullptr);
    bool publish_numeric_history(const EndpointIdentity&,DeliveredNumericFrame,std::uint64_t history_ordinal,std::uint64_t filled_count,QString* error=nullptr);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
