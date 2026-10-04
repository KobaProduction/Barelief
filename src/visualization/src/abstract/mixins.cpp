#include "visualization/abstract/mixins.h"
#include <algorithm>
#include <limits>

void FPSMixin::_calcFPS() {
    if (_fpsCallback) {
        _fpsCallback(static_cast<qreal>(_numberOfCalls) * 1000.0 / _timer.interval());
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
    }
}
