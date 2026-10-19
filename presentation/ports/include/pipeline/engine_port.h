#pragma once
#include "pipeline/engine_models.h"
#include <domain/graph/admission.h>
#include <optional>
#include <QJsonObject>
#include <QHostAddress>
#include <functional>
#include <limits>
namespace pipeline {
struct EngineCommand {
    enum Kind {
        Snapshot = 0,
        ApplyGraph = 1,
        // Retired lifecycle command values remain reserved.
        Subscribe = 8
    } kind = Snapshot;
    QJsonObject graph;
    quint64 expected_revision = std::numeric_limits<quint64>::max();
    int value = 0;
    QHostAddress interface_address = QHostAddress::AnyIPv4, destination = QHostAddress::Broadcast;
    quint16 port = 11988;
    enum Subscription { Views = 1, Audio = 2, History = 4, Previews = 8, Diagnostics = 16, TypedViews = 32 };
    quint32 subscriptions = Views | History | Previews | TypedViews;
};
struct EngineSnapshot {
    quint64 epoch = 0, revision = 0, graph_revision = 0;
    QJsonObject graph;
    QMap<QString, ComponentStatus> statuses;
    // Cold adapter/runtime telemetry; never part of graph parameters.
    QJsonObject diagnostics;
    // Explicit control-plane query owners. Full History here is a cold snapshot;
    // realtime History is delivered exclusively through compact row callbacks.
    std::vector<DeliveredNumericFrame> numeric_frames;
    std::vector<DeliveredNumericHistoryRow> numeric_history_rows;
};
struct EngineDiagnostic {
    domain::ProcessingError error=domain::ProcessingError::None;
    domain::NodeId node=-1;
    domain::PortId port=0;
    std::optional<std::size_t> edge;
    domain::ProcessingReason reason=domain::ProcessingReason::None;
    QString parameter;
    static EngineDiagnostic own(const domain::ProcessingDiagnostic& value) {
        EngineDiagnostic result;
        result.error=value.error;result.node=value.node;result.port=value.port;
        result.edge=value.edge;result.reason=value.reason;
        // Parameter IDs are bounded labels, never retained borrowed transport text.
        if(value.parameter.size()<=128)
            result.parameter=QString::fromUtf8(value.parameter.data(),qsizetype(value.parameter.size()));
        return result;
    }
};

struct EngineReply {
    quint64 request_id = 0;
    bool accepted = false;
    QString error;
    EngineSnapshot snapshot;
    std::optional<EngineDiagnostic> numeric_diagnostic;
};
// Callbacks follow the desktop event-loop delivery contract. Owners destroy
// endpoints before their callback targets; endpoint destruction cancels delivery.
class EnginePort {
public:
    enum class PortDeliveryMode { Unsupported,CanonicalNumericPortsV2 };
    virtual PortDeliveryMode port_delivery_mode() const noexcept {return PortDeliveryMode::Unsupported;}
    // An endpoint advertises NumericPortsV2 only after implementing subscription,
    // ownership and both callbacks.
    virtual bool subscribe_numeric_frames(std::vector<EndpointIdentity>,QString *error=nullptr) {
        if(error)*error=QStringLiteral("Canonical numeric delivery unsupported");return false;
    }
    virtual void set_numeric_frame_handler(std::function<void(DeliveredNumericFrame)>) {}
    virtual void set_numeric_history_handler(std::function<void(DeliveredNumericHistoryRow)>) {}
    // Explicit cold full History resynchronization, with its exact row ordinal.
    // Full History payload may include zero padding. Occupancy is explicit and
    // independent from the matrix's displayed capacity and producer ordinal.
    virtual void set_numeric_history_snapshot_handler(std::function<void(DeliveredNumericFrame,std::uint64_t,std::uint64_t)>) {}
    // Presentation requests complete current values; processing owns all history.
    virtual void request_numeric_snapshots() {}
    // Cold presentation policy. Processing and loss-sensitive taps keep their
    // own cadence; implementations may coalesce preview delivery between ticks.
    virtual void set_numeric_preview_cadence_hz(int) {}
    virtual ~EnginePort() = default;
    virtual quint64 submit(EngineCommand command) = 0;
    virtual void set_snapshot_handler(std::function<void(EngineSnapshot)> handler) = 0;
    virtual void set_reply_handler(std::function<void(EngineReply)> handler) = 0;
    virtual EngineSnapshot snapshot() const = 0;
    virtual bool configure_network(QJsonObject config, QString *error = nullptr) = 0;
    virtual QString network_warning() const = 0;
    virtual void connect_remote() = 0;
    virtual void disconnect_remote() = 0;
};
// Empty host selects the local endpoint. Implementations are assembled by the app.
class EngineProvider {
public:
    virtual ~EngineProvider() = default;
    virtual std::unique_ptr<EnginePort> create(const QString &host, quint16 port) = 0;
};
} // namespace pipeline
