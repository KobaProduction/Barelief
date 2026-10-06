#pragma once
#include <esp_err.h>
#include <freertos/queue.h>
enum uart_port_t {UART_NUM_0,UART_NUM_1,UART_NUM_2,UART_NUM_MAX};
enum uart_event_type_t {UART_DATA,UART_FIFO_OVF,UART_BUFFER_FULL,UART_PARITY_ERR,UART_FRAME_ERR};
struct uart_event_t {uart_event_type_t type;std::size_t size;bool timeout_flag;};
enum {UART_DATA_8_BITS,UART_PARITY_DISABLE,UART_STOP_BITS_1,UART_HW_FLOWCTRL_DISABLE,UART_SCLK_DEFAULT,UART_PIN_NO_CHANGE=-1};
struct uart_config_t {int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk;};
extern "C" {
int uart_read_bytes(uart_port_t,void *,std::uint32_t,TickType_t);
int uart_tx_chars(uart_port_t,const char *,std::uint32_t);
esp_err_t uart_param_config(uart_port_t,const uart_config_t *);
esp_err_t uart_set_pin(uart_port_t,int,int,int,int);
esp_err_t uart_driver_install(uart_port_t,int,int,int,QueueHandle_t *,int);
esp_err_t uart_driver_delete(uart_port_t);
esp_err_t uart_flush_input(uart_port_t);
}
