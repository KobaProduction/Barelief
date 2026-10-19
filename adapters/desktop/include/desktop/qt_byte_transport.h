#pragma once
#include <bdsp_protocol/byte_stream.h>
#include <QIODevice>
#include <QTcpSocket>
#include <QSerialPort>
#include <vector>

namespace desktop {
// Used on the device's owning thread. Qt owns its bounded OS-facing queue;
// PacketStream retains the unsent suffix when a device accepts a partial write.
class QtDeviceByteTransport : public bdsp_protocol::ByteTransport {
public:
    explicit QtDeviceByteTransport(QIODevice &device, qint64 write_budget = 2*1024*1024 + 65536);
    bdsp_protocol::TransportState state() const noexcept override;
    bdsp_protocol::IoResult read(std::span<std::uint8_t>) noexcept override;
    bdsp_protocol::IoResult write(std::span<const std::uint8_t>) noexcept override;
    std::uint64_t overflow_count() const noexcept { return overflows_; }
protected:
    QIODevice &device_;
private:
    qint64 write_budget_;
    std::uint64_t overflows_ = 0;
};
class QtTcpByteTransport final : public QtDeviceByteTransport {
public:
    explicit QtTcpByteTransport(QTcpSocket &, qint64 write_budget = 2*1024*1024 + 65536);
    bdsp_protocol::TransportState state() const noexcept override;
private:
    QTcpSocket &socket_;
};
struct SerialEndpoint {
    QString port_name; // Explicit endpoint; no discovery or implicit default port.
    qint32 baud_rate = 115200;
    QSerialPort::DataBits data_bits = QSerialPort::Data8;
    QSerialPort::Parity parity = QSerialPort::NoParity;
    QSerialPort::StopBits stop_bits = QSerialPort::OneStop;
    QSerialPort::FlowControl flow_control = QSerialPort::NoFlowControl;
};
class QtSerialByteTransport final : public bdsp_protocol::ByteTransport {
public:
    explicit QtSerialByteTransport(qint64 write_budget = 65536);
    signal_contracts::Status open(const SerialEndpoint &);
    void close();
    QSerialPort &device() noexcept { return port_; }
    QString error_string() const { return port_.errorString(); }
    bdsp_protocol::TransportState state() const noexcept override;
    bdsp_protocol::IoResult read(std::span<std::uint8_t>) noexcept override;
    bdsp_protocol::IoResult write(std::span<const std::uint8_t>) noexcept override;
    std::uint64_t overflow_count() const noexcept { return io_.overflow_count(); }
private:
    QSerialPort port_;
    QtDeviceByteTransport io_;
    bool failed_ = false;
};
// Both TCP and UART use this same shared bounded BDSP parser/writer. Device and
// transport must outlive the channel. All calls/signals remain on their Qt thread.
class QtPacketChannel final : public QObject {
    Q_OBJECT
public:
    QtPacketChannel(QIODevice &, bdsp_protocol::ByteTransport &, QObject *parent = nullptr,
                    std::size_t incoming_budget = 65539, std::size_t outgoing_budget = 70000);
    signal_contracts::Status send_packet(quint8, const QByteArray &);
    void reset() noexcept;
    std::size_t pending_bytes() const noexcept { return stream_.pending_bytes(); }
    qint64 device_pending_bytes() const { return device_.bytesToWrite(); }
    std::uint64_t overflow_count() const noexcept { return stream_.overflow_count(); }
Q_SIGNALS:
    void packetReceived(quint8 id, QByteArray payload);
    void protocolError(signal_contracts::Status status);
    void parsingError(BDSP::parse_packet_status_t status);
private:
    void bind_stream_handlers();
    void bind_device_signals();
    void discard_retired_bytes(std::span<std::uint8_t> bytes);
    void feed_received_bytes(std::span<const std::uint8_t> bytes);
    void receive();
    signal_contracts::Status flush();
    QIODevice &device_;
    bdsp_protocol::ByteTransport &transport_;
    std::vector<std::uint8_t> incoming_, outgoing_;
    bdsp_protocol::PacketStream stream_;
    std::uint64_t receive_generation_=0;
};
// Session open/close explicitly fence framing state as well as the OS endpoint.
class QtSerialPacketSession final {
public:
    QtSerialPacketSession() : channel_(transport_.device(), transport_) {}
    signal_contracts::Status open(const SerialEndpoint &endpoint) { channel_.reset(); return transport_.open(endpoint); }
    void close() { channel_.reset(); transport_.close(); }
    QtSerialByteTransport &transport() noexcept { return transport_; }
    QtPacketChannel &channel() noexcept { return channel_; }
private:
    QtSerialByteTransport transport_;
    QtPacketChannel channel_;
};
}
