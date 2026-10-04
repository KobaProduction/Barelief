#include "analyzer/analyze/analyzer.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <atomic>
#include <cstdlib>
#include <new>

static std::atomic<bool> watch_allocations{false};
static std::atomic<unsigned> allocations{0};
void *operator new(std::size_t bytes) {
    if (watch_allocations.load())
        ++allocations;
    if (auto memory = std::malloc(bytes ? bytes : 1))
        return memory;
    throw std::bad_alloc();
}
void operator delete(void *memory) noexcept {
    std::free(memory);
}
void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        Analyzer idle;
        for (int restart = 0; restart < 3; ++restart) {
            idle.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto before_stop = std::chrono::steady_clock::now();
            idle.stop();
            require(std::chrono::steady_clock::now() - before_stop < std::chrono::seconds(1), "Idle Analyzer stop did not wake its PCM wait");
        }
        for (size_t rate : {44100, 48000, 44101}) {
            Analyzer analyzer;
            analyzer.update_sample_rate(rate);
            analyzer.set_mel_enabled(false);
            std::mutex mutex;
            std::condition_variable ready;
            size_t received = 0;
            bool invalid = false;
            analyzer.set_frame_handler([&](const SpectrumFrame &frame) {
                std::lock_guard lock(mutex);
                ++received;
                invalid |= frame.discontinuity || frame.dropped_windows || frame.sample_rate != rate ||
                           !frame.mel_left.empty();
                ready.notify_all();
            });
            analyzer.start();
            // Uneven chunk boundaries must not determine the analysis cadence.
            size_t sent = 0;
            for (size_t batch = 0; batch < 12; ++batch) {
                const size_t end = rate * (batch + 1) / 12;
                while (sent < end) {
                    const auto count = std::min(size_t{317}, end - sent);
                    std::vector<float> pcm(count, 0.1f);
                    sent += count;
                    analyzer.add_samples(pcm, pcm, 1000000000LL + static_cast<int64_t>(sent * 1000000000ULL / rate));
                }
                std::unique_lock lock(mutex);
                require(ready.wait_for(lock, std::chrono::seconds(2), [&] { return received >= (batch + 1) * 5; }),
                        "Audio cadence lost a hop");
            }
            analyzer.stop();
            require(received == 60 && !invalid, "One second does not produce exactly 60 audio frames");
        }
        Analyzer overflow;
        overflow.update_sample_rate(48000);
        std::vector<float> pcm(48000, 0.1f);
        watch_allocations = true;
        overflow.add_samples(pcm, pcm, 2000000000);
        watch_allocations = false;
        require(allocations == 0, "Capture hop queue allocates after construction");
        std::mutex mutex;
        std::condition_variable ready;
        size_t received = 0;
        bool gap = false;
        uint64_t dropped = 0;
        overflow.set_frame_handler([&](const SpectrumFrame &frame) {
            std::lock_guard lock(mutex);
            ++received;
            gap |= frame.discontinuity;
            dropped = frame.dropped_windows;
            ready.notify_all();
        });
        overflow.start();
        {
            std::unique_lock lock(mutex);
            require(ready.wait_for(lock, std::chrono::seconds(2), [&] { return received == 16; }),
                    "Bounded queue did not drain");
        }
        overflow.stop();
        require(gap && dropped == 44 && received == 16, "PCM overflow is unbounded or silent");
        std::cout << "Audio-driven cadence and bounded overflow passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
