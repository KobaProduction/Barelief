#pragma once
#include <bdsp_protocol/byte_stream.h>
#include <driver/uart.h>
#include <freertos/queue.h>
namespace esp_idf_adapter {
// Descriptor/driver lifetime remains host-owned. Calls serialized in transport
// task only. attach sets O_NONBLOCK; no endpoint discovery, connect or close.
class TcpTransport final : public bdsp_protocol::ByteTransport {
public:
    signal_contracts::Status attach(int socket_descriptor) noexcept;
    void detach() noexcept {descriptor_=-1;state_=bdsp_protocol::TransportState::disconnected;}
    bdsp_protocol::TransportState state() const noexcept override {return state_;}
    bdsp_protocol::IoResult read(std::span<std::uint8_t>) noexcept override;
    bdsp_protocol::IoResult write(std::span<const std::uint8_t>) noexcept override;
private:
    int descriptor_=-1;
    bdsp_protocol::TransportState state_=bdsp_protocol::TransportState::disconnected;
};
// Host installs UART driver with TX ring size ZERO (uart_tx_chars writes FIFO
// without waiting). Pass its bounded event queue; overflow terminates session.
// Host must flush/reset driver and queue before attach/reconnect.
class UartTransport final : public bdsp_protocol::ByteTransport {
public:
    signal_contracts::Status attach(uart_port_t, QueueHandle_t events) noexcept;
    void detach() noexcept {events_=nullptr;state_=bdsp_protocol::TransportState::disconnected;}
    bdsp_protocol::TransportState state() const noexcept override {return state_;}
    bdsp_protocol::IoResult read(std::span<std::uint8_t>) noexcept override;
    bdsp_protocol::IoResult write(std::span<const std::uint8_t>) noexcept override;
    std::uint64_t overflow_count() const noexcept {return overflows_;}
private:
    uart_port_t port_=UART_NUM_1;QueueHandle_t events_=nullptr;
    bdsp_protocol::TransportState state_=bdsp_protocol::TransportState::disconnected;
    std::uint64_t overflows_=0;
};
}
