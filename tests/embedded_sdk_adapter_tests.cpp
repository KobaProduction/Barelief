#include <esp_idf_adapter/byte_transport.h>
#include "test_assertions.h"
#define ASSERT(x) test_assertions::require(bool(x),#x,__FILE__,__LINE__)
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
extern "C" BaseType_t xQueueReceive(QueueHandle_t,void *data,TickType_t wait){ASSERT(wait==0);if(!event_pending)return 0;static_cast<uart_event_t *>(data)->type=event_type;event_pending=false;return pdTRUE;}
extern "C" int uart_read_bytes(uart_port_t,void *,std::uint32_t n,TickType_t wait){ASSERT(wait==0);++calls;requested=n;return uart_result;}
extern "C" int uart_tx_chars(uart_port_t,const char *,std::uint32_t n){++calls;requested=n;return uart_result;}
void uart_cases(std::array<std::uint8_t,1024>& bytes){
    esp_idf_adapter::UartTransport uart;
    ASSERT(uart.attach(UART_NUM_1,nullptr)==Status::invalid_argument);
    int event_queue=1;ASSERT(uart.attach(UART_NUM_1,&event_queue)==Status::ok);
    ASSERT(uart.write(bytes).transferred==2 && requested==128);
    ASSERT(uart.read(bytes).transferred==2 && requested==256);
    uart_result=0;ASSERT(uart.write(bytes).status==Status::would_block);
    event_pending=true;ASSERT(uart.read(bytes).status==Status::overflow);ASSERT(uart.overflow_count()==1);
    ASSERT(uart.write(bytes).status==Status::disconnected);
    ASSERT(uart.attach(UART_NUM_1,&event_queue)==Status::ok);
    uart_result=-1;ASSERT(uart.read(bytes).status==Status::disconnected);
    for(auto type:{UART_BUFFER_FULL,UART_PARITY_ERR,UART_FRAME_ERR}) {
        ASSERT(uart.attach(UART_NUM_1,&event_queue)==Status::ok);
        event_pending=true;event_type=type;
        ASSERT(uart.read(bytes).status==(type==UART_BUFFER_FULL?Status::overflow:Status::malformed));
        ASSERT(uart.write(bytes).status==Status::disconnected);
    }
    ASSERT(uart.overflow_count()==2);
    ASSERT(uart.attach(UART_NUM_1,&event_queue)==Status::ok);
    const auto prior_calls=calls;ASSERT(uart.read({}).status==Status::ok && uart.write({}).status==Status::ok && calls==prior_calls);
    uart_result=-1;ASSERT(uart.write(bytes).status==Status::disconnected);
    uart.detach();ASSERT(uart.read(bytes).status==Status::disconnected);
}
int main(){
    std::array<std::uint8_t,1024> bytes{};
    esp_idf_adapter::TcpTransport tcp;
    ASSERT(tcp.read(bytes).status==Status::disconnected);
    fail_flags=true;ASSERT(tcp.attach(7)==Status::disconnected);fail_flags=false;
    ASSERT(tcp.attach(-1)==Status::invalid_argument);ASSERT(tcp.attach(7)==Status::ok);
    const auto initial_calls=calls;ASSERT(tcp.read({}).status==Status::ok && tcp.write({}).status==Status::ok && calls==initial_calls);
    ASSERT(tcp.write(bytes).transferred==3 && requested==256);
    ASSERT(tcp.read(bytes).transferred==3 && requested==256);
    socket_result=-1;errno=EAGAIN;ASSERT(tcp.write(bytes).status==Status::would_block);
    errno=EINTR;ASSERT(tcp.read(bytes).status==Status::would_block);
    errno=EBADF;ASSERT(tcp.write(bytes).status==Status::disconnected);
    ASSERT(tcp.attach(7)==Status::ok);
    socket_result=0;ASSERT(tcp.read(bytes).status==Status::disconnected);
    ASSERT(tcp.write(bytes).status==Status::disconnected);
    uart_cases(bytes);
}
