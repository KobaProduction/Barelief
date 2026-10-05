#ifndef CORE_LATEST_DELIVERY_H
#define CORE_LATEST_DELIVERY_H

#include <QObject>
#include <QMetaObject>
#include <QTimer>
#include <QPointer>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>
#include <algorithm>

// A slow consumer retains only the latest snapshot. Polling avoids posting Qt
// events from an audio/DSP producer; notification mode is for infrequent updates.
// Cancel delivery before destroying the QObject receiver, then stop producers.
template <class T> class LatestDelivery : public std::enable_shared_from_this<LatestDelivery<T>> {
  public:
    LatestDelivery(QObject *receiver, std::function<void(T)> consume, int interval_ms = 0)
        : _receiver(receiver), _consume(std::move(consume)), _interval_ms(interval_ms) {}

    void publish(T value) {
        std::optional<T> retired(std::move(value));
        std::lock_guard lock(_mutex);
        if (!_receiver)
            return;
        retired.swap(_latest);
        if (_polling || _scheduled)
            return;
        _scheduled = true;
        auto consume = [self = this->shared_from_this()] { self->deliver(); };
        QMetaObject::invokeMethod(
            _receiver,
            [self = this->shared_from_this(), consume = std::move(consume)] {
                if (!self->_receiver)
                    return;
                if (self->_interval_ms)
                    QTimer::singleShot(self->_interval_ms, self->_receiver, consume);
                else
                    consume();
            },
            Qt::QueuedConnection);
    }
    // Configure on the receiver thread, after make_shared and before publishing.
    // The receiver owns the timer; a weak capture cannot keep a replaced delivery alive.
    void start_polling(int interval_ms = 17) {
        if (!_receiver || _poll_timer) return;
        {
            std::lock_guard lock(_mutex);
            _polling = true;
        }
        _poll_timer = new QTimer(_receiver);
        _poll_timer->setTimerType(Qt::PreciseTimer);
        _poll_timer->setInterval(std::max(17, interval_ms));
        auto *timer = _poll_timer.data();
        QObject::connect(_poll_timer, &QTimer::timeout, _receiver,
            [weak = this->weak_from_this(), timer] {
                if (auto self = weak.lock()) self->deliver();
                else { timer->stop(); timer->deleteLater(); }
            });
        _poll_timer->start();
    }
    // Called on the receiver thread. In-flight producers finish scheduling under
    // the same lock; subsequent publications never touch a destroyed receiver.
    void cancel() {
        if (_poll_timer) { _poll_timer->stop(); _poll_timer->deleteLater(); _poll_timer = nullptr; }
        std::optional<T> retired;
        {
            std::lock_guard lock(_mutex);
            _receiver = nullptr;
            retired.swap(_latest);
        }
        _consume = {};
    }

  private:
    void deliver() {
        std::optional<T> value;
        {
            std::lock_guard lock(_mutex);
            value.swap(_latest);
            _scheduled = false;
        }
        // A callback may cancel its own delivery; keep its closure alive until return.
        auto consume = _consume;
        if (value && consume) consume(std::move(*value));
    }
    int _interval_ms = 0;
    QObject *_receiver;
    std::function<void(T)> _consume;
    std::mutex _mutex;
    std::optional<T> _latest;
    bool _scheduled = false;
    bool _polling = false;
    QPointer<QTimer> _poll_timer;
};
#endif
