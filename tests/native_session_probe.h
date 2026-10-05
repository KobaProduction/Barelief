#pragma once
#include <QDebug>
#include <QWidget>
#if defined(_WIN32)
#include <windows.h>
#include <wtsapi32.h>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QTimer>
#include <cstring>
#include <cwchar>

// QA provenance only. A locked/disconnected desktop is not a valid interactive
// graphics latency measurement. Never lock/unlock or change the user's session.
class NativeSessionProbe final : public QAbstractNativeEventFilter {
    HWND _window;
    QTimer _poll;
    bool _registered = false, _valid = true;
    unsigned _samples = 0, _changes = 0, _foreground_samples = 0;
    void sample() {
        ++_samples;
        DWORD foreground = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
        if (foreground == GetCurrentProcessId()) ++_foreground_samples;
        if (_samples == 1) {
            SYSTEM_POWER_STATUS power{};
            const bool power_known = GetSystemPowerStatus(&power);
            PROCESS_POWER_THROTTLING_STATE policy{};
            policy.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            using QueryPolicy = BOOL (WINAPI *)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
            const auto query = reinterpret_cast<QueryPolicy>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessInformation"));
            const bool policy_known = query && query(GetCurrentProcess(), ProcessPowerThrottling, &policy, sizeof(policy));
            qInfo("SESSION power_known=%d AC=%u battery=%u policy_known=%d control=%lu state=%lu",
                power_known, unsigned(power.ACLineStatus), unsigned(power.BatteryLifePercent),
                policy_known, policy.ControlMask, policy.StateMask);
        }
        wchar_t name[256]{};
        DWORD bytes = 0;
        const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        const bool desktop_known = desktop && GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &bytes);
        if (desktop) CloseDesktop(desktop);
        LPWSTR info = nullptr; DWORD size = 0;
        const bool session_known = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
            WTSConnectState, &info, &size) && size >= sizeof(WTS_CONNECTSTATE_CLASS);
        WTS_CONNECTSTATE_CLASS state = WTSDown;
        if (session_known) std::memcpy(&state, info, sizeof(state));
        if (info) WTSFreeMemory(info);
        const bool active = desktop_known && std::wcscmp(name, L"Default") == 0 && session_known && state == WTSActive;
        if (_samples == 1 || (!active && _valid))
            qInfo("SESSION desktop=%s connection=%d active=%d", desktop_known ? qPrintable(QString::fromWCharArray(name)) : "unknown", int(state), active);
        _valid &= active;
    }
public:
    explicit NativeSessionProbe(QWidget *window) : _window(reinterpret_cast<HWND>(window->winId())) {
        _registered = WTSRegisterSessionNotification(_window, NOTIFY_FOR_THIS_SESSION);
        if (!_registered) { _valid = false; qWarning("SESSION notification registration failed: %lu", GetLastError()); }
        QCoreApplication::instance()->installNativeEventFilter(this);
        sample();
        _poll.setInterval(1000);
        QObject::connect(&_poll, &QTimer::timeout, &_poll, [this] { sample(); });
        _poll.start();
    }
    ~NativeSessionProbe() override {
        _poll.stop();
        QCoreApplication::instance()->removeNativeEventFilter(this);
        if (_registered) WTSUnRegisterSessionNotification(_window);
    }
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override {
        const auto *event = static_cast<const MSG *>(message);
        if (event->hwnd == _window && event->message == WM_WTSSESSION_CHANGE) {
            ++_changes;
            qInfo("SESSION change=%llu", static_cast<unsigned long long>(event->wParam));
            if (event->wParam == WTS_SESSION_LOCK || event->wParam == WTS_CONSOLE_DISCONNECT ||
                event->wParam == WTS_REMOTE_DISCONNECT || event->wParam == WTS_SESSION_LOGOFF)
                _valid = false;
        }
        return false;
    }
    bool report() {
        _poll.stop(); sample();
        qInfo("SESSION measurement_valid=%d samples=%u changes=%u foreground_samples=%u",
            _valid, _samples, _changes, _foreground_samples);
        return _valid;
    }
};
#else
class NativeSessionProbe {
public:
    explicit NativeSessionProbe(QWidget *) { qInfo("SESSION lock telemetry is only available on Windows"); }
    bool report() { return true; }
};
#endif
