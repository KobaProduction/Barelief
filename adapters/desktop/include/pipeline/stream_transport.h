#pragma once
#include "pipeline/stream_codec.h"
#include <QObject>
#include <QHostAddress>
#include <functional>
#include <memory>
#include <desktop/qt_byte_transport.h>
namespace pipeline {
// Calls and handlers belong to the object's Qt thread. Producers dispatch through a bounded mailbox.
class NetworkPublisher final : public QObject {
  public:
    explicit NetworkPublisher(QObject *parent = nullptr);
    ~NetworkPublisher();
    bool listen(const QHostAddress &address = QHostAddress::LocalHost, quint16 port = 0);
    quint16 port() const;
    void stop();
    void advertise(QString stream_id, stream::Type type);
    bool publish(const QString &stream_id, const stream::Frame &frame);
    uint64_t dropped() const;

  private:
    class Impl;
    std::unique_ptr<Impl> d;
};
class NetworkSubscriber final : public QObject {
  public:
    explicit NetworkSubscriber(QObject *parent = nullptr);
    ~NetworkSubscriber();
    void start(QString host, quint16 port, QString stream_id, stream::Type type);
    // Explicit UART endpoint, using the same typed BDSP subscribe/data contract.
    void start_serial(desktop::SerialEndpoint, QString stream_id, stream::Type type);
    // Attach an explicitly opened packet channel (e.g. a caller-owned UART
    // session). Endpoint lifetime/reopening belongs to the caller. Destruction
    // and Stop fence packet and assembly state; no implicit endpoint discovery.
    void start_channel(desktop::QtPacketChannel &, QString stream_id, stream::Type type);
    void stop();
    void set_frame_handler(std::function<void(stream::Frame)>);
    void set_status_handler(std::function<void(QString)>);

  private:
    class Impl;
    std::unique_ptr<Impl> d;
};
} // namespace pipeline
