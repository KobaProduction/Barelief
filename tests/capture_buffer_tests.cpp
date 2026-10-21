#include "pipeline/capture_buffer.h"
#include "pipeline/capture_wake.h"
#include <future>
#include <thread>
#include <stdexcept>
#include <iostream>
static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static void wake_cases() {

            pipeline::CaptureWake wake;
            for(int i=0;i<10000;++i)wake.notify();
            require(wake.wait_for(std::chrono::milliseconds(0)),"Callback notification before wait was lost");
            require(!wake.wait_for(std::chrono::milliseconds(0)),"Callback bursts did not coalesce");
            std::promise<void> entered;
            auto waiting=std::async(std::launch::async,[&]{entered.set_value();return wake.wait_for(std::chrono::seconds(5));});
            entered.get_future().wait();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            wake.notify();
            const bool timely=waiting.wait_for(std::chrono::milliseconds(500))==std::future_status::ready;
            require(waiting.get()&&timely,"Capture arrival failed to interrupt consumer wait");
            // Exercise ring publication and wake/reset races over many drains.
            std::atomic<unsigned> published=0,consumed=0;
            std::thread producer([&]{for(unsigned i=1;i<=100000;++i){published.store(i,std::memory_order_release);wake.notify();}});
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(consumed.load()<100000&&std::chrono::steady_clock::now()<deadline){
                if(wake.wait_for(std::chrono::milliseconds(20)))
                    consumed.store(published.load(std::memory_order_acquire));
            }
            producer.join();require(consumed==100000,"Wake/reset race stranded published capture data");
        }
static void concurrent_capture_case(unsigned rate,unsigned channels,pipeline::CaptureBuffer& buffer,pipeline::AudioFrame& frame) {
    buffer.reset(rate,channels);
            std::atomic<bool> done{false}; bool corrupt=false;
            std::thread producer([&] {
                std::vector<float> packet(8*channels); auto* pcm=packet.data();
                for (int n = 0; n < 10000; ++n) {
                    for (int i=0;i<8;++i) for(size_t c=0;c<channels;++c) pcm[i*channels+c]=float(n+c*10000);
                    while (!buffer.push(pcm, 8, n, n * 8)) std::this_thread::yield();
                }
                done = true;
            });
            int received = 0;
            while (!done || received < 10000) {
                if (buffer.pop(frame)) {
                    for(size_t c=0;c<channels;++c) corrupt |= frame.samples[c*8] != float(received+c*10000) || frame.samples[c*8+7] != float(received+c*10000);
                    corrupt |= frame.sample_position != uint64_t(received)*8;
                    ++received;
                } else std::this_thread::yield();
            }
            producer.join(); require(!corrupt,"Concurrent ring data/interval corruption");}
static void capacity_case(unsigned rate,unsigned channels) {
            pipeline::CaptureBuffer buffer; buffer.reset(rate, channels);
            const auto capacity = rate / 4;
            std::vector<float> pcm_interleaved(size_t(capacity) * channels);
            for (size_t i = 0; i < capacity; ++i) for (size_t c=0;c<channels;++c) pcm_interleaved[i*channels+c] = float(i+c*capacity);
            require(buffer.push(pcm_interleaved.data(), capacity, 10, 10 * 8), "Capacity block rejected");
            require(!buffer.push(pcm_interleaved.data(), 1, 20, 20 * 8) && buffer.dropped() == 1, "Overflow not bounded/counted");
            pipeline::AudioFrame frame;
            require(buffer.pop(frame) && frame.frame_count == capacity && frame.channel_count == channels && frame.samples.size() == size_t(capacity)*channels && frame.samples.back() == float(channels*capacity-1), "Native channel mapping changed");
            require(buffer.push(pcm_interleaved.data(), 2, 30, 30 * 8), "Recovery block rejected");
            require(buffer.pop(frame) && frame.discontinuity && frame.timestamp_ns == 30 && frame.sample_position == 240, "Drop did not mark next frame");
            require(buffer.push(nullptr, 2, 40, 40 * 8) && buffer.pop(frame) && std::all_of(frame.samples.begin(),frame.samples.end(),[](float x){return x==0;}), "Silence changed");
            buffer.reset(rate, channels);
            require(buffer.push(pcm_interleaved.data(), capacity - 3, 50, 50 * 8) && buffer.pop(frame), "Wrap preparation failed");
            require(buffer.push(pcm_interleaved.data(), 8, 60, 60 * 8) && buffer.pop(frame), "Wrapping packet rejected");
            for (size_t c=0;c<channels;++c) for (size_t i = 0; i < 8; ++i)
                require(frame.samples[c*8+i] == float(i+c*capacity), "Wrapping channel mapping changed");
            buffer.reset(rate, channels);
            require(buffer.push(pcm_interleaved.data(), capacity - 3, 70, 70 * 8) && buffer.pop(frame), "Silence wrap preparation failed");
            require(buffer.push(nullptr, 8, 80, 80 * 8) && buffer.pop(frame), "Wrapping silence rejected");
            for (size_t i = 0; i < 8; ++i)
                require(std::all_of(frame.samples.begin(),frame.samples.end(),[](float x){return x==0;}), "Wrapping silence changed");
            concurrent_capture_case(rate,channels,buffer,frame);
}
int main() {
    try {
        wake_cases();
        for(auto rate:{44100u,48000u})for(auto channels:{1u,2u,6u,8u})capacity_case(rate,channels);
        std::cout << "Capture buffer checks passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
