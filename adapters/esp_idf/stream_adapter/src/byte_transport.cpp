#include <esp_idf_adapter/byte_transport.h>
#include <lwip/sockets.h>
#include <fcntl.h>
#include <cerrno>
#include <algorithm>
namespace esp_idf_adapter {
using signal_contracts::Status;
using bdsp_protocol::TransportState;
using bdsp_protocol::IoResult;
Status TcpTransport::attach(int fd) noexcept {
    detach();if(fd<0)return Status::invalid_argument;
    const auto flags=fcntl(fd,F_GETFL,0);
    if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return Status::disconnected;
    descriptor_=fd;state_=TransportState::connected;return Status::ok;
}
IoResult TcpTransport::read(std::span<std::uint8_t> out) noexcept {
    if(state_!=TransportState::connected)return {Status::disconnected,0};
    if(out.empty())return {Status::ok,0};
    auto n=recv(descriptor_,out.data(),std::min<std::size_t>(out.size(),256),0);
    if(n>0)return {Status::ok,std::size_t(n)};
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return {Status::would_block,0};
    state_=TransportState::failed;return {Status::disconnected,0};
}
IoResult TcpTransport::write(std::span<const std::uint8_t> in) noexcept {
    if(state_!=TransportState::connected)return {Status::disconnected,0};
    if(in.empty())return {Status::ok,0};
    auto n=send(descriptor_,in.data(),std::min<std::size_t>(in.size(),256),0);
    if(n>0)return {Status::ok,std::size_t(n)};
    if(n==0 || (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return {Status::would_block,0};
    state_=TransportState::failed;return {Status::disconnected,0};
}
Status UartTransport::attach(uart_port_t port, QueueHandle_t events) noexcept {
    detach();if(port<0 || port>=UART_NUM_MAX || !events)return Status::invalid_argument;
    port_=port;events_=events;state_=TransportState::connected;return Status::ok;
}
namespace {
Status uart_event_status(const uart_event_t& event) noexcept {
    if(event.type==UART_FIFO_OVF || event.type==UART_BUFFER_FULL)return Status::overflow;
    if(event.type==UART_PARITY_ERR || event.type==UART_FRAME_ERR)return Status::malformed;
    return Status::ok;
}
Status drain_uart_events(QueueHandle_t events) noexcept {
    uart_event_t event;
    // Bounded service: the firmware supplies a queue of <=8 events.
    for(int i=0;i<8 && xQueueReceive(events,&event,0)==pdTRUE;++i) {
        const auto status=uart_event_status(event);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
IoResult UartTransport::read(std::span<std::uint8_t> out) noexcept {
    if(state_!=TransportState::connected)return {Status::disconnected,0};
    const auto status=drain_uart_events(events_);
    if(status==Status::overflow)++overflows_;
    if(status!=Status::ok){state_=TransportState::failed;return {status,0};}
    if(out.empty())return {Status::ok,0};
    const auto n=uart_read_bytes(port_,out.data(),std::min<std::size_t>(out.size(),256),0);
    if(n<0){state_=TransportState::failed;return {Status::disconnected,0};}
    return {n?Status::ok:Status::would_block,std::size_t(n)};
}
IoResult UartTransport::write(std::span<const std::uint8_t> in) noexcept {
    if(state_!=TransportState::connected)return {Status::disconnected,0};
    if(in.empty())return {Status::ok,0};
    const auto n=uart_tx_chars(port_,reinterpret_cast<const char *>(in.data()),std::min<std::size_t>(in.size(),128));
    if(n<0){state_=TransportState::failed;return {Status::disconnected,0};}
    return {n?Status::ok:Status::would_block,std::size_t(n)};
}
}
