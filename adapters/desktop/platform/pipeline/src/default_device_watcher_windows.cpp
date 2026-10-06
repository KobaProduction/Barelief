#include "default_device_watcher_windows.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <thread>
#include <stdexcept>

namespace pipeline {
namespace {
void check(HRESULT result, const char *operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " (HRESULT " + std::to_string(result) + ")");
}
class Notifications final : public IMMNotificationClient {
    std::atomic<ULONG> _references{1};
    std::atomic<bool> _active{true};
    std::atomic<unsigned> _inflight{0};
    std::atomic<bool> &_changed;
    const EDataFlow _flow;
    const std::wstring _id;
    template <typename Predicate> void changed(Predicate relevant) noexcept {
        ++_inflight;
        // Protect endpoint/flow reads as well as flag publication from teardown.
        // Evaluate only after entering the in-flight callback counter.
        if (_active.load() && relevant()) _changed.store(true);
        --_inflight;
    }
public:
    Notifications(EDataFlow flow, std::wstring id, std::atomic<bool> &flag)
        : _changed(flag), _flow(flow), _id(std::move(id)) {}
    void deactivate() { _active = false; }
    void drain() { while (_inflight.load()) std::this_thread::yield(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient *>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++_references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = --_references; if (!count) delete this; return count;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        changed([&] { return flow == _flow && role == eConsole && (!id || _id != id); }); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        changed([&] { return id && _id == id && !(state & DEVICE_STATE_ACTIVE); }); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        changed([&] { return id && _id == id; }); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};
}
struct WindowsDefaultDeviceWatcher::Impl {
    bool com = false, registered = false;
    IMMDeviceEnumerator *enumerator = nullptr;
    Notifications *notifications = nullptr;
    std::wstring id;
    ~Impl() {
        if (notifications) {
            notifications->deactivate();
            if (registered) enumerator->UnregisterEndpointNotificationCallback(notifications);
            notifications->drain();
            notifications->Release();
        }
        if (enumerator) enumerator->Release();
        if (com) CoUninitialize();
    }
};
WindowsDefaultDeviceWatcher::WindowsDefaultDeviceWatcher(bool loopback, std::atomic<bool> &changed)
    : _impl(std::make_unique<Impl>()) {
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize endpoint notifications");
    _impl->com = true;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&_impl->enumerator)), "Create endpoint enumerator");
    const auto flow = loopback ? eRender : eCapture;
    // Register before resolving the endpoint so a change during opening is retained.
    // The initial listener marks any default change; the final listener also watches removal.
    _impl->notifications = new Notifications(flow, {}, changed);
    check(_impl->enumerator->RegisterEndpointNotificationCallback(_impl->notifications), "Watch default endpoint");
    _impl->registered = true;
    IMMDevice *endpoint = nullptr;
    check(_impl->enumerator->GetDefaultAudioEndpoint(flow, eConsole, &endpoint), "Find default endpoint");
    LPWSTR id = nullptr;
    const auto result = endpoint->GetId(&id);
    endpoint->Release();
    check(result, "Read endpoint identifier");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> endpoint_id(id, &CoTaskMemFree);
    _impl->id = endpoint_id.get();
    // The default listener remains registered until the endpoint-specific one is installed.
    auto *specific = new Notifications(flow, _impl->id, changed);
    const auto registration = _impl->enumerator->RegisterEndpointNotificationCallback(specific);
    if (FAILED(registration)) { specific->Release(); check(registration, "Watch capture endpoint"); }
    _impl->notifications->deactivate();
    _impl->enumerator->UnregisterEndpointNotificationCallback(_impl->notifications);
    _impl->notifications->drain();
    _impl->notifications->Release();
    _impl->notifications = specific;
}
WindowsDefaultDeviceWatcher::~WindowsDefaultDeviceWatcher() = default;
const std::wstring &WindowsDefaultDeviceWatcher::device_id() const { return _impl->id; }
}
