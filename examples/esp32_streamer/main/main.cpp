#include "sdkconfig.h"
#include "host_hooks.h"
#include <embedded_support/synthetic_pipeline.h>
#include <esp_idf_adapter/byte_transport.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <array>
using signal_contracts::Status;
extern "C" bool __attribute__((weak)) streamer_host_setup(){return true;}
extern "C" int __attribute__((weak)) streamer_host_acquire_socket(){return -1;}
extern "C" void __attribute__((weak)) streamer_host_release_socket(int){}
extern "C" Status __attribute__((weak)) streamer_host_read_pcm(signal_contracts::FrameView *){return Status::would_block;}
namespace {
constexpr std::size_t FrameBytes=2304, QueueFrames=2;
struct FrameSlot {std::size_t size;std::array<std::uint8_t,FrameBytes> bytes;};
struct Buffers {
    std::array<std::uint8_t,4608> in{},out{};
    std::array<std::uint8_t,256> read{};
    std::array<std::uint8_t,FrameBytes> tx{},assembly{};
    std::array<std::uint8_t,FrameBytes+8> fragment{};
    std::array<float,512> samples{};std::array<char,256> id{};
    embedded_support::SessionBuffers spans(){return {in,out,read,tx,fragment,assembly,samples,id};}
} buffers;
embedded_support::SyntheticPipeline pipeline;
StaticQueue_t queue_control;
alignas(FrameSlot) std::array<std::uint8_t,QueueFrames*sizeof(FrameSlot)> queue_storage;
QueueHandle_t frames=nullptr;
std::atomic<bool> running{false},gap{true};
std::atomic<std::uint64_t> dropped{0};
// ESP-IDF task stack argument is bytes; StaticTask buffer elements are StackType_t.
StaticTask_t dsp_control,transport_control;
StackType_t dsp_stack[8192/sizeof(StackType_t)],transport_stack[8192/sizeof(StackType_t)];
constexpr const char *Tag="portable-stream";
#ifdef CONFIG_STREAMER_SPECTRAL
constexpr bool Spectral=true;
#else
constexpr bool Spectral=false;
#endif
constexpr const char *StreamId=Spectral?"esp32-mel":"esp32-raw";
void capture_output(void *,const signal_contracts::FrameView &view) {
    if(!running)return; // Includes later outputs in the same tick after a fatal marker.
    FrameSlot slot;auto frame=view;frame.metadata.discontinuity|=gap.exchange(false);
    auto result=bdsp_protocol::encode(frame,slot.bytes);
    if(result)slot.size=result.written_bytes;
    if(!result || xQueueSend(frames,&slot,0)!=pdTRUE) {
        gap=true;++dropped;
        // Stop this run instead of losing the control marker behind old data.
        // Only the transport owner may cancel packets/reset queue/close sockets.
        if(!frame.metadata.available)running=false;
    }
}
void cancel_delivery(embedded_support::Session &session) {
    session.stop();xQueueReset(frames);gap=true;
}
void dsp_task(void *) {
    // DSP task owns executor/source; no socket or UART waits here.
    const std::int64_t period=256LL*1000000/8000;
    auto next=esp_timer_get_time();
    while(running) {
        auto now=esp_timer_get_time();
        if(now>=next) {
            Status s;
#ifdef CONFIG_STREAMER_EXTERNAL_PCM
            signal_contracts::FrameView pcm;
            s=streamer_host_read_pcm(&pcm);
            if(s==Status::ok || s==Status::unavailable)s=pipeline.process_pcm(pcm,capture_output,nullptr);
            else if(s==Status::would_block){gap=true;s=Status::ok;}
#else
            s=pipeline.tick(capture_output,nullptr);
#endif
            if(s!=Status::ok){running=false;ESP_LOGE(Tag,"DSP status %d",int(s));break;}
            next+=period;
            if(now-next>period){next=now+period;gap=true;}
        }
        // Scheduler pacing belongs to adapter task, outside graph process().
        vTaskDelay(1);
    }
    vTaskDelete(nullptr);
}
void transport_task(void *) {
    embedded_support::Session session{embedded_support::Role::publisher,
        Spectral?signal_contracts::FrameType::Mel:signal_contracts::FrameType::PCM,StreamId,buffers.spans()};
    esp_idf_adapter::TcpTransport tcp;
    esp_idf_adapter::UartTransport uart;
    bdsp_protocol::ByteTransport *transport=nullptr;
    QueueHandle_t uart_events=nullptr;
#ifdef CONFIG_STREAMER_UART
    uart_config_t config{};
    config.baud_rate=CONFIG_STREAMER_UART_BAUD;config.data_bits=UART_DATA_8_BITS;
    config.parity=UART_PARITY_DISABLE;config.stop_bits=UART_STOP_BITS_1;
    config.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;config.source_clk=UART_SCLK_DEFAULT;
    if(uart_param_config(UART_NUM_1,&config)!=ESP_OK ||
       uart_set_pin(UART_NUM_1,CONFIG_STREAMER_UART_TX,CONFIG_STREAMER_UART_RX,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE)!=ESP_OK ||
       uart_driver_install(UART_NUM_1,4096,0,8,&uart_events,0)!=ESP_OK) {
        running=false;ESP_LOGE(Tag,"UART setup failed");vTaskDelete(nullptr);return;
    }
    transport=&uart;
#else
    transport=&tcp;
#endif
    int descriptor=-1;std::uint64_t epoch=0;
    FrameSlot slot;
    while(running) {
        if(session.state()==embedded_support::SessionState::stopped || session.state()==embedded_support::SessionState::failed) {
            session.stop();xQueueReset(frames);gap=true;
            if(descriptor>=0){tcp.detach();streamer_host_release_socket(descriptor);descriptor=-1;}
#ifdef CONFIG_STREAMER_UART
            uart.detach();uart_flush_input(UART_NUM_1);xQueueReset(uart_events);
            if(uart.attach(UART_NUM_1,uart_events)!=Status::ok)break;
            // Terminate any abandoned partial packet using the unchanged BDSP
            // delimiter. Peer firmware must reset its failed session too.
            constexpr std::array<std::uint8_t,1> delimiter{0};
            if(uart.write(delimiter).transferred!=1){vTaskDelay(1);continue;}
#else
            descriptor=streamer_host_acquire_socket();
            if(descriptor<0){vTaskDelay(pdMS_TO_TICKS(100));continue;}
            if(tcp.attach(descriptor)!=Status::ok){streamer_host_release_socket(descriptor);descriptor=-1;continue;}
#endif
            // Fresh nonzero session epoch, even if two reconnects share a tick.
            const auto clock_epoch=std::uint64_t(esp_timer_get_time())+1;
            epoch=clock_epoch>epoch?clock_epoch:epoch+1;
            if(session.begin(std::uint64_t(esp_timer_get_time()/1000),epoch)!=Status::ok)break;
        }
        const auto status=session.poll(*transport,std::uint64_t(esp_timer_get_time()/1000));
        if(!running)break; // Producer may fence delivery while poll is in progress.
        if(status!=Status::ok && status!=Status::would_block){gap=true;continue;}
        if(xQueueReceive(frames,&slot,0)==pdTRUE) {
            if(session.state()==embedded_support::SessionState::ready) {
                signal_contracts::FrameView frame;
                auto decoded=bdsp_protocol::decode(std::span(slot.bytes).first(slot.size),buffers.samples,buffers.id,frame);
                if(!running)break;
                if(!decoded || session.publish(frame)!=Status::ok){gap=true;++dropped;}
            } else gap=true;
        }
        vTaskDelay(1); // Only adapter task idles; partial writes never block DSP.
    }
    cancel_delivery(session);tcp.detach();uart.detach();
    if(descriptor>=0)streamer_host_release_socket(descriptor);
#ifdef CONFIG_STREAMER_UART
    uart_driver_delete(UART_NUM_1);
#endif
    running=false;vTaskDelete(nullptr);
}
}
extern "C" void app_main() {
    embedded_support::Profile profile;profile.spectral=Spectral;
    // Covers static adapter buffers, two task stacks, driver RX queue, control
    // blocks and example source vectors; queue slots are added separately.
    profile.adapter_bytes=64*1024;
#ifdef CONFIG_STREAMER_UART
    profile.link=embedded_support::Link::uart_8n1;profile.uart_baud=CONFIG_STREAMER_UART_BAUD;
#else
    profile.link=embedded_support::Link::tcp;profile.tcp_bytes_per_second=CONFIG_STREAMER_TCP_BUDGET;
#endif
    const auto admitted=embedded_support::admit(profile,std::char_traits<char>::length(StreamId));
    if(admitted.status!=Status::ok){ESP_LOGE(Tag,"Admission status %d, required %llu B/s",int(admitted.status),(unsigned long long)admitted.required_bytes_per_second);return;}
    const auto prepared=pipeline.prepare(profile,StreamId);
    if(prepared!=Status::ok){ESP_LOGE(Tag,"Prepare status %d",int(prepared));return;}
    if(!streamer_host_setup()){ESP_LOGE(Tag,"Host setup failed");return;}
    frames=xQueueCreateStatic(QueueFrames,sizeof(FrameSlot),queue_storage.data(),&queue_control);
    if(!frames){ESP_LOGE(Tag,"Frame queue failed");return;}
    running=true;
    auto transport=xTaskCreateStatic(transport_task,"bdsp-transport",sizeof(transport_stack),nullptr,4,transport_stack,&transport_control);
    auto dsp=xTaskCreateStatic(dsp_task,"portable-dsp",sizeof(dsp_stack),nullptr,5,dsp_stack,&dsp_control);
    if(!transport || !dsp){running=false;ESP_LOGE(Tag,"Task creation failed");}
}
