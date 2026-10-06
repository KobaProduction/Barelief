#include <esp_idf_adapter/byte_transport.h>
#include <cassert>
#include <array>
#include <algorithm>
#include <cerrno>
using signal_contracts::Status;
namespace {
int socket_result=3,uart_result=2;
bool overflow=false;std::size_t requested=0;
}
extern "C" int fcntl(int,int,...){return 0;}
extern "C" int recv(int,void *,std::size_t n,int){requested=n;return socket_result;}
extern "C" int send(int,const void *,std::size_t n,int){requested=n;return socket_result;}
extern "C" BaseType_t xQueueReceive(QueueHandle_t,void *data,TickType_t wait){assert(wait==0);if(!overflow)return 0;static_cast<uart_event_t *>(data)->type=UART_FIFO_OVF;overflow=false;return pdTRUE;}
extern "C" int uart_read_bytes(uart_port_t,void *,std::uint32_t n,TickType_t wait){assert(wait==0);requested=n;return uart_result;}
extern "C" int uart_tx_chars(uart_port_t,const char *,std::uint32_t n){requested=n;return uart_result;}
int main(){
    std::array<std::uint8_t,1024> bytes{};
    esp_idf_adapter::TcpTransport tcp;
    assert(tcp.attach(-1)==Status::invalid_argument);assert(tcp.attach(7)==Status::ok);
    assert(tcp.write(bytes).transferred==3 && requested==256);
    assert(tcp.read(bytes).transferred==3 && requested==256);
    socket_result=-1;errno=EAGAIN;assert(tcp.write(bytes).status==Status::would_block);
    socket_result=0;assert(tcp.read(bytes).status==Status::disconnected);
    assert(tcp.write(bytes).status==Status::disconnected);
    esp_idf_adapter::UartTransport uart;
    assert(uart.attach(UART_NUM_1,nullptr)==Status::invalid_argument);
    int event_queue=1;assert(uart.attach(UART_NUM_1,&event_queue)==Status::ok);
    assert(uart.write(bytes).transferred==2 && requested==128);
    assert(uart.read(bytes).transferred==2 && requested==256);
    uart_result=0;assert(uart.write(bytes).status==Status::would_block);
    overflow=true;assert(uart.read(bytes).status==Status::overflow);assert(uart.overflow_count()==1);
    assert(uart.write(bytes).status==Status::disconnected);
    assert(uart.attach(UART_NUM_1,&event_queue)==Status::ok);
    uart_result=-1;assert(uart.read(bytes).status==Status::disconnected);
}
