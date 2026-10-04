#ifndef CORE_LATEST_DELIVERY_H
#define CORE_LATEST_DELIVERY_H

#include <QObject>
#include <QMetaObject>
#include <QTimer>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>

// A slow consumer retains only the latest snapshot and one queued notification.
// Cancel delivery before destroying the QObject receiver, then stop producers.
template <class T> class LatestDelivery : public std::enable_shared_from_this<LatestDelivery<T>> {
  public:
    LatestDelivery(QObject *receiver, std::function<void(T)> consume, int interval_ms = 0)
        : _receiver(receiver), _consume(std::move(consume)), _interval_ms(interval_ms) {}

    void publish(T value) {
        std::lock_guard lock(_mutex);
        if (!_receiver)
            return;
        _latest = std::move(value);
        if (_scheduled)
            return;
        _scheduled = true;
        auto consume = [self = this->shared_from_this()] {
            std::optional<T> value;
            {
                std::lock_guard lock(self->_mutex);
                value.swap(self->_latest);
                self->_scheduled = false;
            }
            if (value && self->_consume)
                self->_consume(std::move(*value));
        };
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
    // Called on the receiver thread. In-flight producers finish scheduling under
    // the same lock; subsequent publications never touch a destroyed receiver.
    void cancel() {
        std::lock_guard lock(_mutex);
        _receiver = nullptr;
        _consume = {};
        _latest.reset();
    }

  private:
    int _interval_ms = 0;
    QObject *_receiver;
    std::function<void(T)> _consume;
    std::mutex _mutex;
    std::optional<T> _latest;
    bool _scheduled = false;
};
#endif
