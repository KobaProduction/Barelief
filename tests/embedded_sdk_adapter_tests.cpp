#include <esp_idf_adapter/byte_transport.h>
#include <cassert>
#include <array>
#include <algorithm>
#include <cerrno>
using signal_contracts::Status;
namespace {
int socket_result=3,uart_result=2;
bool event_pending=false,fail_flags=false;uart_event_type_t event_type=UART_FIFO_OVF;
std::size_t requested=0,calls=0;
}
extern "C" int fcntl(int,int,...){return fail_flags?-1:0;}
extern "C" int recv(int,void *,std::size_t n,int){++calls;requested=n;return socket_result;}
extern "C" int send(int,const void *,std::size_t n,int){++calls;requested=n;return socket_result;}
extern "C" BaseType_t xQueueReceive(QueueHandle_t,void *data,TickType_t wait){assert(wait==0);if(!event_pending)return 0;static_cast<uart_event_t *>(data)->type=event_type;event_pending=false;return pdTRUE;}
extern "C" int uart_read_bytes(uart_port_t,void *,std::uint32_t n,TickType_t wait){assert(wait==0);++calls;requested=n;return uart_result;}
extern "C" int uart_tx_chars(uart_port_t,const char *,std::uint32_t n){++calls;requested=n;return uart_result;}
int main(){
    std::array<std::uint8_t,1024> bytes{};
    esp_idf_adapter::TcpTransport tcp;
    assert(tcp.read(bytes).status==Status::disconnected);
    fail_flags=true;assert(tcp.attach(7)==Status::disconnected);fail_flags=false;
    assert(tcp.attach(-1)==Status::invalid_argument);assert(tcp.attach(7)==Status::ok);
    const auto initial_calls=calls;assert(tcp.read({}).status==Status::ok && tcp.write({}).status==Status::ok && calls==initial_calls);
    assert(tcp.write(bytes).transferred==3 && requested==256);
    assert(tcp.read(bytes).transferred==3 && requested==256);
    socket_result=-1;errno=EAGAIN;assert(tcp.write(bytes).status==Status::would_block);
    errno=EINTR;assert(tcp.read(bytes).status==Status::would_block);
    errno=EBADF;assert(tcp.write(bytes).status==Status::disconnected);
    assert(tcp.attach(7)==Status::ok);
    socket_result=0;assert(tcp.read(bytes).status==Status::disconnected);
    assert(tcp.write(bytes).status==Status::disconnected);
    esp_idf_adapter::UartTransport uart;
    assert(uart.attach(UART_NUM_1,nullptr)==Status::invalid_argument);
    int event_queue=1;assert(uart.attach(UART_NUM_1,&event_queue)==Status::ok);
    assert(uart.write(bytes).transferred==2 && requested==128);
    assert(uart.read(bytes).transferred==2 && requested==256);
    uart_result=0;assert(uart.write(bytes).status==Status::would_block);
    event_pending=true;assert(uart.read(bytes).status==Status::overflow);assert(uart.overflow_count()==1);
    assert(uart.write(bytes).status==Status::disconnected);
    assert(uart.attach(UART_NUM_1,&event_queue)==Status::ok);
    uart_result=-1;assert(uart.read(bytes).status==Status::disconnected);
    for(auto type:{UART_BUFFER_FULL,UART_PARITY_ERR,UART_FRAME_ERR}) {
        assert(uart.attach(UART_NUM_1,&event_queue)==Status::ok);
        event_pending=true;event_type=type;
        assert(uart.read(bytes).status==(type==UART_BUFFER_FULL?Status::overflow:Status::malformed));
        assert(uart.write(bytes).status==Status::disconnected);
    }
    assert(uart.overflow_count()==2);
    assert(uart.attach(UART_NUM_1,&event_queue)==Status::ok);
    const auto prior_calls=calls;assert(uart.read({}).status==Status::ok && uart.write({}).status==Status::ok && calls==prior_calls);
    uart_result=-1;assert(uart.write(bytes).status==Status::disconnected);
    uart.detach();assert(uart.read(bytes).status==Status::disconnected);
}
