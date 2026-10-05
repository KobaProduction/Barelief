#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace pipeline {
// Discovery may wait on a server. Keep it off the PCM delivery owner, and join
// before destroying its captured state. The query must honor its stop token.
class DefaultDevicePoller final {
    std::jthread _worker;
public:
    using Query = std::function<bool(std::stop_token)>;
    DefaultDevicePoller(std::atomic<bool> &reopen, Query query,
                        std::chrono::milliseconds interval = std::chrono::seconds(1))
        : _worker([&reopen, query = std::move(query), interval](std::stop_token stop) {
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            while (!stop.stop_requested()) {
                wake.wait_for(lock, stop, interval, [] { return false; });
                if (stop.stop_requested()) return;
                bool changed = true;
                try { changed = query(stop); }
                catch (...) { /* Rediscover through the ordinary recovery path. */ }
                if (!stop.stop_requested() && changed) {
                    reopen.store(true, std::memory_order_release);
                    return;
                }
            }
        }) {}
    ~DefaultDevicePoller() { stop(); }
    void stop() {
        _worker.request_stop();
        if (_worker.joinable()) _worker.join();
    }
};
}
