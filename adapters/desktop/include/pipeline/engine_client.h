#pragma once
#include "pipeline/audio_pipeline.h"
#include "pipeline/engine_port.h"
#include <QObject>
#include <limits>
namespace pipeline {
class EngineClient final : public QObject, public EnginePort {
  public:
    explicit EngineClient(QObject *parent = nullptr);
    explicit EngineClient(AudioSourceFactory, QObject *parent = nullptr);
    EngineClient(QString host, quint16 port, QObject *parent = nullptr);
    ~EngineClient() override;
    PortDeliveryMode port_delivery_mode() const noexcept override;
    bool subscribe_port_frames(std::vector<EndpointIdentity>,QString *error=nullptr) override;
    void set_port_frame_handler(std::function<void(DeliveredPortFrame)>) override;
    quint64 submit(EngineCommand command) override;
    void set_snapshot_handler(std::function<void(EngineSnapshot)> handler) override;
    void set_view_handler(std::function<void(ViewFrame)> handler) override;
    void set_audio_handler(std::function<void(AudioFrame)> handler) override;
    void set_reply_handler(std::function<void(EngineReply)> handler) override;
    EngineSnapshot snapshot() const override;
    AudioPipeline *local_engine() const;
    bool configure_network(QJsonObject config, QString *error = nullptr) override;
    QString network_warning() const override;
    void connect_remote() override;
    void disconnect_remote() override;

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
