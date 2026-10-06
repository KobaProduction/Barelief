#pragma once
#include <atomic>
#include <memory>
#include <string>

namespace pipeline {
// Endpoint notifications only; device lifetime belongs to the capture worker.
class WindowsDefaultDeviceWatcher {
public:
    WindowsDefaultDeviceWatcher(bool loopback, std::atomic<bool> &changed);
    ~WindowsDefaultDeviceWatcher();
    const std::wstring &device_id() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
