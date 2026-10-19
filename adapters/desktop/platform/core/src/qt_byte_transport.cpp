#include <desktop/qt_byte_transport.h>
#include <algorithm>
#include <array>
#include <limits>

namespace desktop {
using signal_contracts::Status;
using bdsp_protocol::TransportState;
QtDeviceByteTransport::QtDeviceByteTransport(QIODevice &device, qint64 budget)
    : device_(device), write_budget_(std::max(qint64(1), budget)) {}
TransportState QtDeviceByteTransport::state() const noexcept {
    return device_.isOpen() ? TransportState::connected : TransportState::disconnected;
}
bdsp_protocol::IoResult QtDeviceByteTransport::read(std::span<std::uint8_t> destination) noexcept {
    if(state()!=TransportState::connected || !device_.isReadable()) return {Status::disconnected,0};
    if(destination.empty()) return {Status::ok,0};
    if(!device_.bytesAvailable()) return {Status::would_block,0};
    const auto limit=std::min(destination.size(),std::size_t(std::numeric_limits<qint64>::max()));
    const auto count=device_.read(reinterpret_cast<char *>(destination.data()),qint64(limit));
    if(count<0) return {Status::unavailable,0};
    return {count?Status::ok:Status::would_block,std::size_t(count)};
}
bdsp_protocol::IoResult QtDeviceByteTransport::write(std::span<const std::uint8_t> source) noexcept {
    if(state()!=TransportState::connected || !device_.isWritable()) return {Status::disconnected,0};
    if(source.empty()) return {Status::ok,0};
    const auto room=write_budget_-device_.bytesToWrite();
    // A full device queue is reversible backpressure: the packet owner retains
    // its unwritten bytes and retries after Qt drains the socket/UART.
    if(room<=0) { ++overflows_; return {Status::would_block,0}; }
    const auto count=device_.write(reinterpret_cast<const char *>(source.data()),qint64(std::min(source.size(),std::size_t(room))));
    if(count<0) return {Status::unavailable,0};
    return {count?Status::ok:Status::would_block,std::size_t(count)};
}
QtTcpByteTransport::QtTcpByteTransport(QTcpSocket &socket,qint64 budget)
    : QtDeviceByteTransport(socket,budget),socket_(socket) { socket_.setReadBufferSize(65536); }
TransportState QtTcpByteTransport::state() const noexcept {
    switch(socket_.state()) {
    case QAbstractSocket::ConnectedState:return TransportState::connected;
    case QAbstractSocket::HostLookupState:case QAbstractSocket::ConnectingState:return TransportState::connecting;
    default:
        return socket_.error()!=QAbstractSocket::UnknownSocketError && socket_.error()!=QAbstractSocket::RemoteHostClosedError
            ? TransportState::failed : TransportState::disconnected;
    }
}
QtSerialByteTransport::QtSerialByteTransport(qint64 budget):io_(port_,budget) { port_.setReadBufferSize(65536); }
Status QtSerialByteTransport::open(const SerialEndpoint &endpoint) {
    close();
    if(endpoint.port_name.trimmed().isEmpty() || endpoint.baud_rate<=0) return Status::invalid_argument;
    port_.setPortName(endpoint.port_name);
    if(!port_.setBaudRate(endpoint.baud_rate) || !port_.setDataBits(endpoint.data_bits) ||
       !port_.setParity(endpoint.parity) || !port_.setStopBits(endpoint.stop_bits) || !port_.setFlowControl(endpoint.flow_control)) {
        failed_=true; return Status::invalid_argument;
    }
    failed_=!port_.open(QIODevice::ReadWrite);
    return failed_?Status::unavailable:Status::ok;
}
void QtSerialByteTransport::close() { port_.close();failed_=false; }
TransportState QtSerialByteTransport::state() const noexcept {
    if(failed_ || (port_.isOpen() && (port_.error()==QSerialPort::ResourceError || port_.error()==QSerialPort::ReadError || port_.error()==QSerialPort::WriteError))) return TransportState::failed;
    return io_.state();
}
bdsp_protocol::IoResult QtSerialByteTransport::read(std::span<std::uint8_t> bytes) noexcept {
    return state()==TransportState::failed?bdsp_protocol::IoResult{Status::unavailable,0}:io_.read(bytes);
}
bdsp_protocol::IoResult QtSerialByteTransport::write(std::span<const std::uint8_t> bytes) noexcept {
    return state()==TransportState::failed?bdsp_protocol::IoResult{Status::unavailable,0}:io_.write(bytes);
}
QtPacketChannel::QtPacketChannel(QIODevice &device,bdsp_protocol::ByteTransport &transport,QObject *parent,
                               std::size_t incoming_budget,std::size_t outgoing_budget)
    : QObject(parent),device_(device),transport_(transport),
      incoming_(std::clamp(incoming_budget,std::size_t(1),std::size_t(65539))),
      outgoing_(std::clamp(outgoing_budget,std::size_t(1),std::size_t(70000))),stream_(incoming_,outgoing_) {
    bind_stream_handlers();
    bind_device_signals();
}
void QtPacketChannel::bind_stream_handlers() {
    stream_.set_packet_handler([](bdsp_protocol::PacketView packet,void *context) {
        auto *self=static_cast<QtPacketChannel *>(context);
        emit self->packetReceived(packet.id,QByteArray(reinterpret_cast<const char *>(packet.payload.data()),qsizetype(packet.payload.size())));
    },this);
    stream_.set_error_handler([](BDSP::parse_packet_status_t status,void *context) {
        emit static_cast<QtPacketChannel *>(context)->parsingError(status);
    },this);
}
void QtPacketChannel::bind_device_signals() {
    connect(&device_,&QIODevice::readyRead,this,&QtPacketChannel::receive);
    connect(&device_,&QIODevice::bytesWritten,this,&QtPacketChannel::flush);
    connect(&device_,&QIODevice::aboutToClose,this,&QtPacketChannel::reset);
    if(auto *socket=qobject_cast<QAbstractSocket *>(&device_)) {
        connect(socket,&QAbstractSocket::connected,this,&QtPacketChannel::reset);
        connect(socket,&QAbstractSocket::disconnected,this,&QtPacketChannel::reset);
    }
    if(auto *serial=qobject_cast<QSerialPort *>(&device_)) {
        connect(serial,&QSerialPort::errorOccurred,this,[this](QSerialPort::SerialPortError error) {
            if(error==QSerialPort::ResourceError || error==QSerialPort::ReadError || error==QSerialPort::WriteError) {
                reset(); emit protocolError(Status::unavailable);
            }
        });
    }
}
void QtPacketChannel::discard_retired_bytes(std::span<std::uint8_t> bytes) {
    // A callback retired this session. Fence the suffix still buffered
    // by QIODevice as well as the current PacketStream feed chunk.
    while(device_.bytesAvailable()>0) {
        const auto discarded=transport_.read(bytes);
        if(!discarded.transferred)break;
    }
}
void QtPacketChannel::reset() noexcept { ++receive_generation_;stream_.reset(); }
Status QtPacketChannel::send_packet(quint8 id,const QByteArray &payload) {
    if(transport_.state()!=TransportState::connected) return Status::disconnected;
    const auto result=stream_.enqueue(id,{reinterpret_cast<const std::uint8_t *>(payload.constData()),std::size_t(payload.size())});
    if(result!=Status::ok)return result;
    const auto status=flush();
    // A full device queue or zero-byte partial write leaves the accepted packet
    // queued. A terminal device failure must not claim acceptance to the caller.
    return status==Status::unavailable || status==Status::disconnected?status:Status::ok;
}
void QtPacketChannel::feed_received_bytes(std::span<const std::uint8_t> bytes) {
    const auto status=stream_.feed(bytes);
    if(status!=Status::ok)emit protocolError(status);
}
void QtPacketChannel::receive() {
    std::array<std::uint8_t,4096> bytes;
    while(true) {
        const auto generation=receive_generation_;
        const auto result=transport_.read(bytes);
        if(result.transferred)feed_received_bytes(std::span(bytes).first(result.transferred));
        if(generation!=receive_generation_) {
            discard_retired_bytes(std::span(bytes));
            break;
        }
        if(result.status==Status::unavailable)emit protocolError(result.status);
        if(!result.transferred || result.status!=Status::ok)break;
    }
}
Status QtPacketChannel::flush() {
    while(stream_.pending_bytes()) {
        const auto result=stream_.flush(transport_);
        if(result.status==Status::unavailable || result.status==Status::disconnected) { stream_.reset();emit protocolError(result.status);return result.status; }
        if(!result.transferred || result.status!=Status::ok)return result.status;
    }
    return Status::ok;
}
}
