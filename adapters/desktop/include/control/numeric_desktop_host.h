#pragma once
#include <pipeline/engine_port.h>
#include <pipeline/audio_source.h>
#include <desktop/qt_byte_transport.h>
#include <QObject>
#include <QUuid>
#include <memory>
namespace pipeline {
class NumericEngine;
// Desktop composition boundary. Qt transports, control and processing lifetime
// belong to a worker event loop; GUI notifications use bounded owning mailboxes.
class NumericDesktopHost final:public QObject,public EnginePort {
public:
 explicit NumericDesktopHost(AudioSourceFactory={},QString host={},quint16 port=8213,
     std::optional<desktop::SerialEndpoint> serial={},QObject* parent=nullptr);
 ~NumericDesktopHost() override;
 NumericEngine* local_engine() const;
 // Owner-thread local capability check reads the immutable execution profile
 // without waiting for the worker. The host must outlive the synchronous call.
 // Remote unknown capabilities remain authoritative at Apply; this query does
 // not impose the desktop profile.
 domain::ProcessingDiagnostic validate_numeric_node(const domain::ConfiguredProcessingNode&) const;
 // Zero refuses admission before dispatch when all128 completion slots are
 // outstanding. Every admitted nonzero request keeps its correlated reply.
 quint64 submit(EngineCommand) override;
 void set_snapshot_handler(std::function<void(EngineSnapshot)>) override;
 void set_reply_handler(std::function<void(EngineReply)>) override;
 EngineSnapshot snapshot()const override;
 bool configure_network(QJsonObject,QString* error=nullptr) override;
 QString network_warning()const override;
 void connect_remote()override;void disconnect_remote()override;
 PortDeliveryMode port_delivery_mode()const noexcept override;
 bool subscribe_numeric_frames(std::vector<EndpointIdentity>,QString* error=nullptr)override;
 void set_numeric_frame_handler(std::function<void(DeliveredNumericFrame)>)override;
 void set_numeric_history_handler(std::function<void(DeliveredNumericHistoryRow)>)override;
 void set_numeric_history_snapshot_handler(std::function<void(DeliveredNumericFrame,std::uint64_t,std::uint64_t)>)override;
 void request_numeric_snapshots() override;
 void set_numeric_preview_cadence_hz(int) override;
 void configure_registry(QHostAddress,quint16,int timeout_ms=1000);
 void disconnect_registry();
 void set_registry_capabilities(QJsonObject);
 void set_registry_identity(QUuid);
 signal_contracts::Status feed_numeric(domain::NodeId,const signal_contracts::DataFrameView&);
 signal_contracts::Status feed_numeric_owned(domain::NodeId,const signal_contracts::OwnedDataFrame&,
     std::optional<std::uint64_t> ordinal={},std::shared_ptr<const void> owner={},std::optional<std::uint64_t> filled_count={});
 signal_contracts::Status query_numeric(const EndpointIdentity&,DeliveredNumericFrame&,std::uint64_t* ordinal=nullptr,std::uint64_t* filled_count=nullptr);
 bool listen(QHostAddress,quint16,bool allow_mutations=true);
 quint16 server_port()const;
 void close_server();
 QJsonObject routing_diagnostics()const;
private:
 struct Impl;std::unique_ptr<Impl> impl_;
};
}
