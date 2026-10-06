#include "pipeline/test_signal.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <thread>

namespace pipeline {
QJsonObject TestSignalSettings::json() const {
    return {{"tones", tones}, {"sample_rate", sample_rate}, {"minimum_hz", minimum_hz},
        {"maximum_hz", maximum_hz}, {"amplitude", amplitude},
        {"period_seconds", period_seconds}, {"sweep_depth", sweep_depth}};
}
bool TestSignalSettings::parse(const QJsonObject &json, TestSignalSettings &settings, QString &error) {
    TestSignalSettings result;
    const auto defaults = result.json();
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const auto value = json.value(it.key());
        if (!value.isUndefined() && (!value.isDouble() || !std::isfinite(value.toDouble()))) {
            error = "TestSignal parameter must be a finite number: " + it.key(); return false;
        }
    }
    const auto tones = json.value("tones").toDouble(8);
    const auto rate = json.value("sample_rate").toDouble(48000);
    if (tones != std::floor(tones) || tones < 5 || tones > 10 ||
        rate != std::floor(rate) || rate < 8000 || rate > 192000) {
        error = "TestSignal requires 5–10 tones and an integer sample rate of 8000–192000 Hz"; return false;
    }
    result.tones = int(tones); result.sample_rate = int(rate);
    result.minimum_hz = json.value("minimum_hz").toDouble(73);
    result.maximum_hz = json.value("maximum_hz").toDouble(13901);
    result.amplitude = json.value("amplitude").toDouble(0.4);
    result.period_seconds = json.value("period_seconds").toDouble(6);
    result.sweep_depth = json.value("sweep_depth").toDouble(0.18);
    if (result.minimum_hz < 10 || result.maximum_hz <= result.minimum_hz ||
        result.amplitude < 0 || result.amplitude > 1 ||
        result.period_seconds < 0.1 || result.period_seconds > 3600 ||
        result.sweep_depth < 0 || result.sweep_depth > 0.95 ||
        result.maximum_hz * (1 + result.sweep_depth) >= result.sample_rate / 2.0) {
        error = "Invalid TestSignal range, amplitude or sweep (frequencies must stay below Nyquist)"; return false;
    }
    settings = result; error.clear(); return true;
}
namespace {
// An in-memory producer, not a playback device. Never creates a miniaudio
// context/device or routes generated PCM to the operating system's output.
class TestSignalSource final : public IAudioSource {
    TestSignalSettings _settings;
    AudioHandler _audio;
    StatusHandler _status;
    LifecycleHandler _lifecycle;
    std::atomic<bool> _running{false};
    std::jthread _worker;
    std::mutex _wait_mutex;
    std::condition_variable _wake;
public:
    ~TestSignalSource() override { stop(); }
    void configure(const QJsonObject &json) override {
        if (_worker.joinable()) throw std::logic_error("Stop TestSignal before configuring it");
        QString error;
        if (!TestSignalSettings::parse(json, _settings, error)) throw std::invalid_argument(error.toStdString());
    }
    void start() override {
        stop();
        _worker = std::jthread([this](std::stop_token stop) {
            try {
                std::array<double,10> carriers{}, phase{};
                for (int tone = 0; tone < _settings.tones; ++tone)
                    carriers[tone] = _settings.minimum_hz * std::pow(_settings.maximum_hz / _settings.minimum_hz,
                        double(tone) / (_settings.tones - 1));
                const auto origin = std::chrono::steady_clock::now();
                auto deadline = origin;
                const auto block = size_t(std::max(1, _settings.sample_rate / 200));
                uint64_t offset = 0, sequence = 0;
                _running = true;
                if (_lifecycle) _lifecycle(true, 0, {});
                if (_status) _status(true, {});
                while (!stop.stop_requested()) {
                    AudioFrame frame;
                    frame.sample_rate = _settings.sample_rate; frame.sequence = ++sequence;
                    frame.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(origin.time_since_epoch()).count() +
                        int64_t(offset * 1000000000ULL / _settings.sample_rate);
                    frame.discontinuity = offset == 0;
                    frame.left.resize(block); frame.right.resize(block);
                    for (size_t sample = 0; sample < block; ++sample) {
                        const double time = double(offset + sample) / _settings.sample_rate;
                        double left = 0, right = 0;
                        for (int tone = 0; tone < _settings.tones; ++tone) {
                            const double cycle = 2 * std::numbers::pi * (tone + 1) / _settings.period_seconds;
                            const double hz = carriers[tone] * (1 + _settings.sweep_depth * std::sin(cycle * time));
                            const double gain = _settings.amplitude / _settings.tones *
                                (0.65 + 0.35 * std::sin(cycle * time + tone));
                            left += gain * std::sin(phase[tone]);
                            right += gain * std::sin(phase[tone] + (tone + 1) * 0.31);
                            phase[tone] = std::remainder(phase[tone] + 2 * std::numbers::pi * hz / _settings.sample_rate,
                                2 * std::numbers::pi);
                        }
                        frame.left[sample] = float(left); frame.right[sample] = float(right);
                    }
                    offset += block;
                    if (stop.stop_requested()) break;
                    if (_audio) _audio(std::move(frame));
                    deadline = origin + std::chrono::nanoseconds(offset * 1000000000ULL / _settings.sample_rate);
                    std::unique_lock lock(_wait_mutex);
                    _wake.wait_until(lock, deadline, [&] { return stop.stop_requested(); });
                }
            } catch (const std::exception &error) {
                if (_status) _status(false, QString::fromUtf8(error.what()));
                if (_lifecycle) _lifecycle(false, 1, QString::fromUtf8(error.what()));
            }
            _running = false;
        });
    }
    void stop() override {
        _worker.request_stop(); _wake.notify_all();
        if (_worker.joinable()) _worker.join();
        _running = false;
    }
    bool is_running() const override { return _running; }
    AudioSourceKind source() const override { return AudioSourceKind::TestSignal; }
    AudioSourceDiagnostics diagnostics() const override {
        AudioSourceDiagnostics result; result.backend = "In-memory test generator";
        result.device = "TestSignal (no audio output)"; result.sample_rate = _settings.sample_rate; return result;
    }
    void set_audio_handler(AudioHandler handler) override { _audio = std::move(handler); }
    void set_status_handler(StatusHandler handler) override { _status = std::move(handler); }
    void set_lifecycle_handler(LifecycleHandler handler) override { _lifecycle = std::move(handler); }
};
}
std::unique_ptr<IAudioSource> make_test_signal_source() { return std::make_unique<TestSignalSource>(); }
}
