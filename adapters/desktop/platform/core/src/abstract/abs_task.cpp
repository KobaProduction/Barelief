#include "core/abstract/abs_task.h"
#include <chrono>
#include <QDebug>
AbstractTask::~AbstractTask() { stop(); }
void AbstractTask::start() {
    if (_running.load()) return;
    if (_thread.joinable()) _thread.join();
    _running = true;
    try {
        _thread = std::thread([this] { _run_task(); });
    } catch (...) {
        _running = false;
        throw;
    }
}
void AbstractTask::_run_task() {
    uint64_t result = 0;
    std::string message;
    try {
        result = _before_run_task();
        if (!result && _running.load()) {
            _notify(true, 0, {});
            result = _task();
        }
    } catch (const std::exception &error) { result = 1; message = error.what(); }
    catch (...) { result = 1; message = "Unknown worker exception"; }
    try { _after_close_task(); }
    catch (const std::exception &error) { result = 1; message = error.what(); }
    catch (...) { result = 1; message = "Unknown cleanup exception"; }
    _running = false;
    _notify(false, result, message);
}
void AbstractTask::stop() {
    _running = false;
    _wake.notify_all();
    if (_thread.joinable()) _thread.join();
}
uint64_t AbstractTask::_before_run_task() { return 0; }
void AbstractTask::_sleep(float ms) {
    if (ms <= 0) return;
    std::unique_lock lock(_sleep_mutex);
    _wake.wait_for(lock, std::chrono::duration<float, std::milli>(ms), [this] { return !_running.load(); });
}

void AbstractTask::set_lifecycle_handler(std::function<void(bool, uint64_t, const std::string &)> handler) {
    std::lock_guard lock(_lifecycle_mutex); _lifecycle_handler = std::move(handler);
}
void AbstractTask::_notify(bool ready, uint64_t result, const std::string &error) {
    std::function<void(bool, uint64_t, const std::string &)> handler;
    { std::lock_guard lock(_lifecycle_mutex); handler = _lifecycle_handler; }
    if (handler) {
        try { handler(ready, result, error); }
        catch (...) { qCritical("Worker lifecycle observer failed"); }
    }
}
