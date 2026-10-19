#ifndef BARELIEF_MIXINS_H
#define BARELIEF_MIXINS_H

#include <functional>
#include <QTimer>
#include <QElapsedTimer>

class FPSMixin {
    typedef std::function<void(qreal)> FPSCallback;
public:
    explicit FPSMixin(const QObject *parent);

    void setFPSTimeout(quint64 timeoutMs);

    void setFPSCallback(FPSCallback callback);

protected:
    ~FPSMixin() = default;

    void _frameCall();

private:
    QTimer _timer;
    QElapsedTimer _fps_clock;
    FPSCallback _fpsCallback = nullptr;
    quint64 _numberOfCalls = 0;

    void _calcFPS();
};


#endif //BARELIEF_MIXINS_H
