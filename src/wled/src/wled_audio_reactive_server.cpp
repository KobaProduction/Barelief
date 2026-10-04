#include "core/performance.h"
#include <QHostAddress>
#include <QUdpSocket>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "wled/wled_audio_reactive_server.h"


Q_LOGGING_CATEGORY(wled_audio_reactive_server, "wled_audio_reactive_server")

WLEDAudioReactiveServer::WLEDAudioReactiveServer(const QHostAddress &broadcast_address, const quint16 broadcast_port, const QHostAddress &destination) {
    // Store interface address for binding
    _interface_address = broadcast_address;
    
    // Always use limited broadcast address (255.255.255.255) for WLED
    _broadcast_address = destination;
    _broadcast_port = broadcast_port;
    _data.resize(16, 0.0);
    _frame_counter = 0;

    _udp_socket = new QUdpSocket();

    // Bind to specific interface address for broadcast to work on Windows
    // The source IP must be the interface IP for broadcast to be sent correctly
    const bool bind_ok = _udp_socket->bind(_interface_address, 0);
    _bound = bind_ok;
    if (!bind_ok) _error = _udp_socket->errorString();

    if (not bind_ok) {
        qCWarning(wled_audio_reactive_server, "Failed to bind socket to %s: %s",
                 _interface_address.toString().toStdString().c_str(),
                 _udp_socket->errorString().toStdString().c_str());
    } else {
        qCInfo(wled_audio_reactive_server, "Socket bound to interface: %s", _interface_address.toString().toStdString().c_str());
    }

    _timer.setInterval(17);
    _timer.setTimerType(Qt::PreciseTimer);
    _last_update.start();
    connect(&_timer, &QTimer::timeout, this, [this] {
        if (_last_update.elapsed() > 300) {
            std::fill(_data.begin(), _data.end(), 0); _peak_hz = 0; _magnitude = 0;
        }
        _handler(); _need_update = false;
    });

    qCInfo(wled_audio_reactive_server, "UDP socket created and configured for broadcast");

    qCInfo(wled_audio_reactive_server, "WLED broadcast address set to: %s:%u (interface: %s)",
           _broadcast_address.toString().toStdString().c_str(), _broadcast_port,
           _interface_address.toString().toStdString().c_str());
}

WLEDAudioReactiveServer::~WLEDAudioReactiveServer() {
    stop();
    delete _udp_socket;
}

void WLEDAudioReactiveServer::update(const std::vector<float> &spectrum, float peak_hz, float magnitude) {
    _peak_hz = peak_hz; _magnitude = magnitude; _last_update.restart();
    if (spectrum.size() != 50) {
        qCInfo(wled_audio_reactive_server, "incorrect size: expected 50, got %zu", spectrum.size());
        return;
    }
    
    // Map spectrum[4..46] (42 values) to 16 WLED bins
    // Clamp mapped magnitudes to the byte range when encoding the packet.
    constexpr int start_idx = 4;
    constexpr int end_idx = 46;
    constexpr int useful_bins = end_idx - start_idx;  // 42 bins
    constexpr int target_bins = 16;
    
    // Calculate bins per target bin
    constexpr float bins_per_target = static_cast<float>(useful_bins) / target_bins;
    
    for (int i = 0; i < target_bins; ++i) {
        // Calculate source range for this target bin
        const float src_start = i * bins_per_target;
        const float src_end = (i + 1) * bins_per_target;
        
        // Average values in source range
        float sum = 0.0f;
        int count = 0;
        
        for (int j = static_cast<int>(src_start); j < static_cast<int>(src_end) && (start_idx + j) < end_idx; ++j) {
            const float value = spectrum[start_idx + j];
            sum += std::isfinite(value) ? std::max(0.0f, value) : 0.0f;
            count++;
        }
        
        // Preserve level changes; saturation is handled by the packet encoder.
        _data[i] = count > 0 ? sum / count : 0.0f;
    }
    
    _need_update = true;
}

void WLEDAudioReactiveServer::start() { if (_bound) _timer.start(); }
void WLEDAudioReactiveServer::stop() { _timer.stop(); }

wled_audio_reactive::AudioSyncPacket WLEDAudioReactiveServer::make_packet(
    const std::vector<float> &bins, float peak_hz, float magnitude, uint8_t counter) {
    wled_audio_reactive::AudioSyncPacket packet{};
    std::memcpy(packet.header, "00002", 6);
    packet.frameCounter = counter;
    float sum = 0, peak = 0;
    for (size_t i = 0; i < 16; ++i) {
        float value = i < bins.size() && std::isfinite(bins[i]) ? std::clamp(bins[i], 0.0f, 1.0f) : 0;
        sum += value; peak = std::max(peak, value);
        packet.fftResult[i] = static_cast<uint8_t>(value * 255);
    }
    packet.sampleRaw = packet.sampleSmth = sum / 16 * 255;
    packet.samplePeak = peak > 0.5f;
    packet.FFT_Magnitude = std::isfinite(magnitude) ? std::max(0.0f, magnitude) : 0;
    packet.FFT_MajorPeak = std::isfinite(peak_hz) ? std::max(0.0f, peak_hz) : 0;
    return packet;
}
void WLEDAudioReactiveServer::_handler() {
    core::perf::Scope trace(core::perf::Stage::WledSend);
    const auto packet = make_packet(_data, _peak_hz, _magnitude, _frame_counter++);
    const auto sent = _udp_socket->writeDatagram(reinterpret_cast<const char *>(&packet), sizeof(packet), _broadcast_address, _broadcast_port);
    if (sent != sizeof(packet)) {
        _error = _udp_socket->errorString();
        qCWarning(wled_audio_reactive_server, "UDP send failed: %s", qPrintable(_error));
        _timer.stop();
        if (_error_handler) _error_handler(_error);
    }
}
