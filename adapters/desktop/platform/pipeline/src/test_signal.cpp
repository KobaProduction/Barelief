#include "pipeline/test_signal.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <limits>
#include <thread>

namespace pipeline {
bool test_signal_sample_time_ns(std::uint64_t samples,std::uint32_t rate,std::int64_t origin_ns,std::int64_t& timestamp_ns) noexcept {
    if(!rate)return false;
    const auto seconds=samples/rate,fraction=(samples%rate)*1000000000ULL/rate;
    const auto maximum=std::uint64_t(origin_ns>0?std::numeric_limits<std::int64_t>::max()-origin_ns:std::numeric_limits<std::int64_t>::max());
    if(fraction>maximum || seconds>(maximum-fraction)/1000000000ULL)return false;
    timestamp_ns=origin_ns+std::int64_t(seconds*1000000000ULL+fraction);return true;
}

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
    mutable std::mutex _settings_mutex;
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
    bool reconfigure_live(const QJsonObject& json) override {
        TestSignalSettings next;QString error;
        if(!TestSignalSettings::parse(json,next,error))return false;
        std::lock_guard lock(_settings_mutex);
        if(next.sample_rate!=_settings.sample_rate)return false;
        _settings=next;
        return true;
    }
    void start() override {
        stop();
        _worker = std::jthread([this](std::stop_token stop) {
            try {
                std::array<double,10> carriers{}, phase{};
                TestSignalSettings settings;
                {std::lock_guard lock(_settings_mutex);settings=_settings;}
                const auto origin = std::chrono::steady_clock::now();
                auto deadline = origin;
                const auto origin_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(origin.time_since_epoch()).count();
                const auto elapsed=[&](std::uint64_t samples,int rate) {
                    std::int64_t timestamp;
                    if(!test_signal_sample_time_ns(samples,std::uint32_t(rate),origin_ns,timestamp))throw std::overflow_error("TestSignal sample clock exhausted");
                    return std::chrono::nanoseconds(timestamp-origin_ns);
                };
                const auto block = size_t(std::max(1, settings.sample_rate / 200));
                uint64_t offset = 0, sequence = 0;
                _running = true;
                if (_lifecycle) _lifecycle(true, 0, {});
                if (_status) _status(true, {});
                while (!stop.stop_requested()) {
                    {std::lock_guard lock(_settings_mutex);settings=_settings;}
                    for(int tone=0;tone<settings.tones;++tone)
                        carriers[tone]=settings.minimum_hz*std::pow(settings.maximum_hz/settings.minimum_hz,double(tone)/(settings.tones-1));
                    AudioFrame frame;
                    frame.sample_rate = settings.sample_rate; frame.sequence = ++sequence;
                    frame.timestamp_ns = origin_ns + elapsed(offset,settings.sample_rate).count();
                    frame.discontinuity = offset == 0;
                    frame.left.resize(block); frame.right.resize(block);
                    for (size_t sample = 0; sample < block; ++sample) {
                        const double time = double(offset + sample) / settings.sample_rate;
                        double left = 0, right = 0;
                        for (int tone = 0; tone < settings.tones; ++tone) {
                            const double cycle = 2 * std::numbers::pi * (tone + 1) / settings.period_seconds;
                            const double hz = carriers[tone] * (1 + settings.sweep_depth * std::sin(cycle * time));
                            const double gain = settings.amplitude / settings.tones *
                                (0.65 + 0.35 * std::sin(cycle * time + tone));
                            left += gain * std::sin(phase[tone]);
                            right += gain * std::sin(phase[tone] + (tone + 1) * 0.31);
                            phase[tone] = std::remainder(phase[tone] + 2 * std::numbers::pi * hz / settings.sample_rate,
                                2 * std::numbers::pi);
                        }
                        frame.left[sample] = float(left); frame.right[sample] = float(right);
                    }
                    if(offset>std::numeric_limits<std::uint64_t>::max()-block)throw std::overflow_error("TestSignal sample counter exhausted");
                    offset += block;
                    if (stop.stop_requested()) break;
                    if (_audio) _audio(std::move(frame));
                    deadline = origin + elapsed(offset,settings.sample_rate);
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
        std::lock_guard lock(_settings_mutex);
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
