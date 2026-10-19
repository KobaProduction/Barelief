#include "barelief/abstract/mixins.h"
#include <algorithm>
#include <limits>

void FPSMixin::_calcFPS() {
    if (_fpsCallback) {
        const auto elapsed=_fps_clock.nsecsElapsed();
        _fps_clock.restart();
        _fpsCallback(elapsed>0 ? static_cast<qreal>(_numberOfCalls) * 1e9 / elapsed : 0);
    }
    _numberOfCalls = 0;
}

void FPSMixin::_frameCall() {
    _numberOfCalls++;
}

FPSMixin::FPSMixin(const QObject *parent) {

    QObject::connect(&_timer, &QTimer::timeout, parent, [this] {
        _calcFPS();
    });
    _timer.setInterval(1000);
}

void FPSMixin::setFPSTimeout(const quint64 timeoutMs) {
    _timer.setInterval(static_cast<int>(std::clamp<quint64>(timeoutMs, 1, std::numeric_limits<int>::max())));
}

void FPSMixin::setFPSCallback(FPSCallback callback) {
    _fpsCallback = std::move(callback);
    if (not _fpsCallback) {
        _timer.stop();
    } else {
        _timer.start();
        _numberOfCalls = 0;
        _fps_clock.start();
    }
}
