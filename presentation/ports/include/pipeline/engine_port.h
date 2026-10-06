#pragma once
#include "pipeline/engine_models.h"
#include <QJsonObject>
#include <QHostAddress>
#include <functional>
#include <limits>
namespace pipeline {
struct EngineCommand {
    enum Kind {
        Snapshot,
        ApplyGraph,
        StartAnalyzer,
        StopAnalyzer,
        StartCapture,
        StopCapture,
        StartWled,
        StopWled,
        SetMerge,
        SetDisplayChannel,
        Subscribe
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
    quint64 analyzer_generation = 0, capture_generation = 0;
    QJsonObject graph;
    QMap<QString, ComponentStatus> statuses;
    std::shared_ptr<const HistorySnapshot> history;
    std::vector<float> window_signal, window, windowed_signal;
    float frequency_step = 0;
};
struct EngineReply {
    quint64 request_id = 0;
    bool accepted = false;
    QString error;
    EngineSnapshot snapshot;
};
// Callbacks follow the desktop event-loop delivery contract. Owners destroy
// endpoints before their callback targets; endpoint destruction cancels delivery.
class EnginePort {
public:
    virtual ~EnginePort() = default;
    virtual quint64 submit(EngineCommand command) = 0;
    virtual void set_snapshot_handler(std::function<void(EngineSnapshot)> handler) = 0;
    virtual void set_view_handler(std::function<void(ViewFrame)> handler) = 0;
    virtual void set_audio_handler(std::function<void(AudioFrame)> handler) = 0;
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
