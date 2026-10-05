#include "pipeline/capture_buffer.h"
#include <thread>
#include <stdexcept>
#include <iostream>
static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        for (auto rate : {44100u, 48000u}) {
            pipeline::CaptureBuffer buffer; buffer.reset(rate);
            const auto capacity = rate / 4;
            std::vector<float> stereo(capacity * 2);
            for (size_t i = 0; i < capacity; ++i) { stereo[i*2] = float(i); stereo[i*2+1] = -float(i); }
            require(buffer.push(stereo.data(), capacity, 10), "Capacity block rejected");
            require(!buffer.push(stereo.data(), 1, 20) && buffer.dropped() == 1, "Overflow not bounded/counted");
            pipeline::AudioFrame frame;
            require(buffer.pop(frame) && frame.left.size() == capacity && frame.right.back() == -float(capacity-1), "Stereo changed");
            require(buffer.push(stereo.data(), 2, 30), "Recovery block rejected");
            require(buffer.pop(frame) && frame.discontinuity && frame.timestamp_ns == 30, "Drop did not mark next frame");
            require(buffer.push(nullptr, 2, 40) && buffer.pop(frame) && frame.left[1] == 0 && frame.right[1] == 0, "Silence changed");
            buffer.reset(rate);
            require(buffer.push(stereo.data(), capacity - 3, 50) && buffer.pop(frame), "Wrap preparation failed");
            require(buffer.push(stereo.data(), 8, 60) && buffer.pop(frame), "Wrapping packet rejected");
            for (size_t i = 0; i < 8; ++i)
                require(frame.left[i] == float(i) && frame.right[i] == -float(i), "Wrapping stereo changed");
            buffer.reset(rate);
            require(buffer.push(stereo.data(), capacity - 3, 70) && buffer.pop(frame), "Silence wrap preparation failed");
            require(buffer.push(nullptr, 8, 80) && buffer.pop(frame), "Wrapping silence rejected");
            for (size_t i = 0; i < 8; ++i)
                require(frame.left[i] == 0 && frame.right[i] == 0, "Wrapping silence changed");
            buffer.reset(rate);
            std::atomic<bool> done{false};
            std::thread producer([&] {
                float pcm[16];
                for (int n = 0; n < 10000; ++n) {
                    for (int i=0;i<8;++i) { pcm[2*i]=float(n); pcm[2*i+1]=-float(n); }
                    while (!buffer.push(pcm, 8, n)) std::this_thread::yield();
                }
                done = true;
            });
            int received = 0;
            while (!done || received < 10000) {
                if (buffer.pop(frame)) {
                    require(frame.left[0] == received && frame.right[7] == -received, "Concurrent ring corruption");
                    ++received;
                } else std::this_thread::yield();
            }
            producer.join();
        }
        std::cout << "Capture buffer checks passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
