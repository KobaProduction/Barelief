#pragma once
#include <atomic>
#include <chrono>
#include <semaphore>

namespace pipeline {
// Desktop callback-to-consumer notification. The consumer drains its SPSC ring
// after each wait; notifications coalesce without retaining audio or allocating.
class CaptureWake {
    std::binary_semaphore ready_{0};
    std::atomic<bool> pending_{false};
public:
    void notify() noexcept {
        if (!pending_.exchange(true, std::memory_order_acq_rel)) ready_.release();
    }
    bool wait_for(std::chrono::milliseconds timeout) {
        if (!ready_.try_acquire_for(timeout)) return false;
        // A producer racing this store has already published its data, which
        // the immediately following drain consumes even if its wake coalesced.
        // Acquire the latest coalesced producer notification as well as the
        // original semaphore release, before the consumer inspects its ring.
        pending_.exchange(false, std::memory_order_acq_rel);
        return true;
    }
};
}
