#include "../src/pipeline/src/default_device_poller.h"
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
static bool await(const std::atomic<bool> &value) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!value && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    return value.load();
}
int main() {
    try {
        std::atomic<bool> reopen{false}, entered{false}, finished{false};
        const auto owner = std::this_thread::get_id();
        pipeline::DefaultDevicePoller slow(reopen, [&](std::stop_token stop) {
            require(std::this_thread::get_id() != owner, "Discovery ran on its delivery owner");
            entered = true;
            // An unresponsive server waits until cancellation; the owner stays runnable.
            while (!stop.stop_requested()) std::this_thread::sleep_for(1ms);
            finished = true;
            return true;
        }, 1ms);
        require(await(entered), "Discovery did not start");
        for (int tick = 0; tick < 10; ++tick) std::this_thread::sleep_for(1ms);
        require(!finished && !reopen, "Pending discovery changed the device prematurely");
        auto start = std::chrono::steady_clock::now();
        slow.stop();
        require(std::chrono::steady_clock::now() - start < 200ms, "Pending discovery did not cancel promptly");
        require(finished && !reopen, "Cancelled discovery requested a stale reopen");
        std::atomic<unsigned> calls{0};
        pipeline::DefaultDevicePoller sleeping(reopen, [&](std::stop_token) { ++calls; return true; }, 5s);
        start = std::chrono::steady_clock::now(); sleeping.stop();
        require(std::chrono::steady_clock::now() - start < 200ms && !calls,
                "Stop did not cancel the discovery interval");
        pipeline::DefaultDevicePoller changed(reopen, [&](std::stop_token) { return ++calls == 3; }, 1ms);
        require(await(reopen), "Changed default device did not request recovery");
        changed.stop(); require(calls == 3, "Discovery continued after requesting recovery");
        reopen = false;
        pipeline::DefaultDevicePoller failed(reopen, [](std::stop_token) -> bool {
            throw std::runtime_error("Unavailable server");
        }, 1ms);
        require(await(reopen), "Discovery failure did not request recovery");
        failed.stop();
        std::cout << "Asynchronous discovery, change/failure and cancellation checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
