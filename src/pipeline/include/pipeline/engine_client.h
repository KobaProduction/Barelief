#pragma once
#include "pipeline/audio_pipeline.h"
#include <QObject>
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
    enum Subscription { Views = 1, Audio = 2, History = 4, Previews = 8, Diagnostics = 16 };
    quint32 subscriptions = Views | History | Previews;
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
class EngineClient final : public QObject {
  public:
    explicit EngineClient(QObject *parent = nullptr);
    EngineClient(QString host, quint16 port, QObject *parent = nullptr);
    ~EngineClient() override;
    quint64 submit(EngineCommand command);
    void set_snapshot_handler(std::function<void(EngineSnapshot)> handler);
    void set_view_handler(std::function<void(ViewFrame)> handler);
    void set_audio_handler(std::function<void(AudioFrame)> handler);
    void set_reply_handler(std::function<void(EngineReply)> handler);
    EngineSnapshot snapshot() const;
    AudioPipeline *local_engine() const;
    void connect_remote();
    void disconnect_remote();

  private:
    class Impl;
    std::unique_ptr<Impl> d;
};
class EngineServer final : public QObject {
  public:
    explicit EngineServer(AudioPipeline &engine, QObject *parent = nullptr);
    ~EngineServer() override;
    bool listen(const QHostAddress &address = QHostAddress::LocalHost, quint16 port = 8213,
                bool allow_mutations = false);
    quint16 port() const;
    QString error() const;

  private:
    class Impl;
    std::unique_ptr<Impl> d;
};
} // namespace pipeline
