#include "pipeline/test_signal.h"
#include <array>
#include <dsp_kernels/oscillator_bank.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <limits>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
// Platform pacing belongs to the source adapter, not the sample clock/DSP.
// Standard timed condition-variable waits can quantize short deadlines on
// Windows. A private high-resolution timer avoids changing global resolution.
class ProducerDeadlineWait {
    std::mutex mutex_;
    std::condition_variable wake_;
    bool cancelled_=false;
#ifdef _WIN32
    HANDLE timer_=nullptr,stop_=nullptr;
#endif
public:
    ProducerDeadlineWait() {
#ifdef _WIN32
        timer_=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_MODIFY_STATE|SYNCHRONIZE);
        stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!timer_||!stop_) {
            if(timer_)CloseHandle(timer_);
            if(stop_)CloseHandle(stop_);
            timer_=stop_=nullptr;
        }
#endif
    }
    ProducerDeadlineWait(const ProducerDeadlineWait&)=delete;
    ~ProducerDeadlineWait() {
#ifdef _WIN32
        if(timer_)CloseHandle(timer_);
        if(stop_)CloseHandle(stop_);
#endif
    }
    void cancel() noexcept {
        {std::lock_guard lock(mutex_);cancelled_=true;}
#ifdef _WIN32
        if(stop_)SetEvent(stop_);
#endif
        wake_.notify_all();
    }
    void until(std::chrono::steady_clock::time_point deadline) {
#ifdef _WIN32
        if(timer_) {
            const auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-std::chrono::steady_clock::now()).count();
            if(remaining<=0)return;
            LARGE_INTEGER due{};due.QuadPart=-((remaining-1)/100+1);
            if(SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE)) {
                const HANDLE handles[]{stop_,timer_};
                const auto result=WaitForMultipleObjects(2,handles,FALSE,INFINITE);
                if(result==WAIT_OBJECT_0||result==WAIT_OBJECT_0+1)return;
            }
        }
#endif
        std::unique_lock lock(mutex_);
        wake_.wait_until(lock,deadline,[this]{return cancelled_;});
    }
};
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
                ProducerDeadlineWait wait;
                // Destroy the callback before its referenced wait handles.
                std::stop_callback cancellation(stop,[&wait]{wait.cancel();});
                std::array<dsp_kernels::OscillatorParameters,10> tones{};
                dsp_kernels::OscillatorBank bank;
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
                AudioFrame frame;frame.channel_count=1;frame.frame_count=block;frame.samples.resize(block);
                bool prepared=false;
                _running = true;
                if (_lifecycle) _lifecycle(true, 0, {});
                if (_status) _status(true, {});
                while (!stop.stop_requested()) {
                    {std::lock_guard lock(_settings_mutex);settings=_settings;}
                    for(int tone=0;tone<settings.tones;++tone) {
                        const auto carrier=settings.minimum_hz*std::pow(settings.maximum_hz/settings.minimum_hz,double(tone)/(settings.tones-1));
                        const auto cycle=2*std::numbers::pi*(tone+1)/settings.period_seconds;
                        const auto time=double(offset)/settings.sample_rate;
                        tones[tone]={std::uint64_t(tone+1),true,carrier*(1+settings.sweep_depth*std::sin(cycle*time)),
                            settings.amplitude/settings.tones*(0.65+0.35*std::sin(cycle*time+tone)),0,0,0};
                    }
                    const auto parameters=std::span(tones).first(settings.tones);
                    if(!(prepared?bank.update(parameters):bank.prepare(parameters,settings.sample_rate)))
                        throw std::runtime_error("TestSignal oscillator admission failed");
                    prepared=true;
                    frame.sample_rate=settings.sample_rate;frame.sequence=++sequence;frame.sample_position=offset;
                    frame.timestamp_ns=origin_ns+elapsed(offset,settings.sample_rate).count();frame.discontinuity=offset==0;
                    if(!bank.render(frame.samples))throw std::runtime_error("TestSignal produced nonfinite data");
                    if(offset>std::numeric_limits<std::uint64_t>::max()-block)throw std::overflow_error("TestSignal sample counter exhausted");
                    offset += block;
                    if (stop.stop_requested()) break;
                    if (_audio) _audio(frame);
                    deadline = origin + elapsed(offset,settings.sample_rate);
                    wait.until(deadline);
                }
            } catch (const std::exception &error) {
                if (_status) _status(false, QString::fromUtf8(error.what()));
                if (_lifecycle) _lifecycle(false, 1, QString::fromUtf8(error.what()));
            }
            _running = false;
        });
    }
    void stop() override {
        _worker.request_stop();
        if (_worker.joinable()) _worker.join();
        _running = false;
    }
    bool is_running() const override { return _running; }
    AudioSourceKind source() const override { return AudioSourceKind::TestSignal; }
    AudioSourceDiagnostics diagnostics() const override {
        std::lock_guard lock(_settings_mutex);
        AudioSourceDiagnostics result; result.backend = "In-memory test generator";
        result.device = "TestSignal (no audio output)"; result.sample_rate = result.native_sample_rate = _settings.sample_rate; result.native_channels = result.delivered_channels = 1; return result;
    }
    void set_audio_handler(AudioHandler handler) override { _audio = std::move(handler); }
    void set_status_handler(StatusHandler handler) override { _status = std::move(handler); }
    void set_lifecycle_handler(LifecycleHandler handler) override { _lifecycle = std::move(handler); }
};
}
std::unique_ptr<IAudioSource> make_test_signal_source() { return std::make_unique<TestSignalSource>(); }
}
