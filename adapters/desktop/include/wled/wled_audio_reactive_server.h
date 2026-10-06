#ifndef WLED_AUDIO_REACTIVE_SERVER_H
#define WLED_AUDIO_REACTIVE_SERVER_H

#include <QTimer>
#include <QElapsedTimer>
#include <functional>
#include <qloggingcategory.h>
#include <QHostAddress>
#include <QUdpSocket>

#include "core/abstract/abs_task.h"
#include "abstract/types.h"

Q_DECLARE_LOGGING_CATEGORY(wled_audio_reactive_server);

class WLEDAudioReactiveServer final: public QObject, public ITask {
    std::vector<float> _data;
    QHostAddress _interface_address;
    QHostAddress _broadcast_address;
    quint16 _broadcast_port;
    QUdpSocket *_udp_socket = nullptr;
    uint8_t _frame_counter = 0;
    QTimer _timer;
    bool _need_update = false;
    bool _bound = false;
    float _peak_hz = 0, _magnitude = 0;
    QElapsedTimer _last_update;
    QString _error;
    std::function<void(QString)> _error_handler;
public:
    explicit WLEDAudioReactiveServer(const QHostAddress &broadcast_address, quint16 broadcast_port, const QHostAddress &destination = QHostAddress::Broadcast);
    ~WLEDAudioReactiveServer() override;

    void update(const std::vector<float>& spectrum, float peak_hz = 0, float magnitude = 0);
    void set_error_handler(std::function<void(QString)> handler) { _error_handler = std::move(handler); }
    bool ready() const { return _bound; }
    QString error() const { return _error; }
    static wled_audio_reactive::AudioSyncPacket make_packet(const std::vector<float> &bins, float peak_hz, float magnitude, uint8_t counter);

    void start() override;
    void stop() override;
protected:
    void _handler();
};


#endif // WLED_AUDIO_REACTIVE_SERVER_H
