// Compile the actual example callback and transport cleanup against the bounded
// fake queue. This is not a copied implementation of its admission behavior.
#include SDK_EXAMPLE_MAIN
#include <cassert>
#include <deque>
namespace {std::deque<FrameSlot> queued;}
extern "C" BaseType_t xQueueSend(QueueHandle_t,const void *slot,TickType_t wait){assert(wait==0);if(queued.size()==QueueFrames)return 0;queued.push_back(*static_cast<const FrameSlot *>(slot));return pdTRUE;}
extern "C" BaseType_t xQueueReceive(QueueHandle_t,void *slot,TickType_t wait){assert(wait==0);if(queued.empty())return 0;*static_cast<FrameSlot *>(slot)=queued.front();queued.pop_front();return pdTRUE;}
extern "C" BaseType_t xQueueReset(QueueHandle_t){queued.clear();return pdTRUE;}
extern "C" QueueHandle_t xQueueCreateStatic(std::size_t,std::size_t,std::uint8_t *,StaticQueue_t *){return &queue_control;}
extern "C" void vTaskDelay(TickType_t){}
extern "C" void vTaskDelete(TaskHandle_t){}
extern "C" TaskHandle_t xTaskCreateStatic(void (*)(void *),const char *,std::uint32_t,void *,unsigned,StackType_t *,StaticTask_t *){return &dsp_control;}
extern "C" std::int64_t esp_timer_get_time(){return 1;}
extern "C" int recv(int,void *,std::size_t,int){return 0;}
extern "C" int send(int,const void *,std::size_t,int){return 0;}
extern "C" int fcntl(int,int,...){return 0;}
extern "C" int uart_read_bytes(uart_port_t,void *,std::uint32_t,TickType_t){return 0;}
extern "C" int uart_tx_chars(uart_port_t,const char *,std::uint32_t){return 0;}
int main(){
    static_assert(FftSize==CONFIG_STREAMER_FFT_SIZE);
    static_assert(buffers.samples.size()==2*BlockSamples);
    const auto profile=example_profile();
    assert(profile.fft_size==CONFIG_STREAMER_FFT_SIZE && profile.memory_bytes==MemoryBytes);
    assert(profile.block_samples==BlockSamples && profile.adapter_bytes==64*1024);
    frames=&queue_control;running=true;gap=false;dropped=0;
    std::array<float,4> pcm{};
    signal_contracts::FrameView f;f.metadata.stream_id=StreamId;f.metadata.sample_rate=8000;f.metadata.sequence=1;f.left=pcm;f.right=pcm;
    capture_output(nullptr,f);++f.metadata.sequence;capture_output(nullptr,f);assert(queued.size()==2);
    // Ordinary saturation drops a complete frame; processing remains alive.
    ++f.metadata.sequence;capture_output(nullptr,f);assert(running && gap && dropped==1);
    f.metadata.available=false;f.metadata.sequence=0;f.left={};f.right={};
    capture_output(nullptr,f);
    assert(!running); // Marker must fence the old queue, never disappear as data.
    const auto before=dropped.load();
    f.metadata.available=true;f.metadata.sequence=4;f.left=pcm;f.right=pcm;
    capture_output(nullptr,f);assert(queued.size()==2 && dropped==before);
    // Execute actual transport-task exit: queued data is purged by its owner.
    transport_task(nullptr);assert(queued.empty() && !running);
    capture_output(nullptr,f);assert(queued.empty());
    // Same cleanup also cancels encoded packets already pending in Session.
    embedded_support::Session pending{embedded_support::Role::subscriber,signal_contracts::FrameType::PCM,StreamId,buffers.spans()};
    assert(pending.begin(0,0)==Status::ok && pending.pending_bytes()>0);
    cancel_delivery(pending);assert(pending.pending_bytes()==0 && pending.state()==embedded_support::SessionState::stopped);
}
