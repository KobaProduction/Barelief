#include "pipeline/audio_source.h"
#include "pipeline/test_signal.h"
#include "pipeline/capture_buffer.h"
#include "core/performance.h"
#include <miniaudio.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstring>
#include <stdexcept>
#if defined(_WIN32)
#include "default_device_watcher_windows.h"
#endif
#if defined(__linux__)
#include <pulse/pulseaudio.h>
#include "default_device_poller.h"
#endif

namespace pipeline {
namespace {
uint64_t clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#if defined(__linux__)
struct PulseSource { QString name; uint32_t rate; };
PulseSource pulse_default_source(AudioSourceKind kind, const std::function<bool()> &cancelled) {
    struct Query { QString source, sink; uint32_t rate = 0; bool server = false, done = false; } query;
    auto *loop = pa_mainloop_new();
    if (!loop) throw std::runtime_error("Cannot allocate PulseAudio mainloop");
    auto *context = pa_context_new(pa_mainloop_get_api(loop), "qt-visualization capture discovery");
    if (!context) { pa_mainloop_free(loop); throw std::runtime_error("Cannot create PulseAudio context"); }
    const auto cleanup = [&] { pa_context_disconnect(context); pa_context_unref(context); pa_mainloop_free(loop); };
    const auto deadline = clock_ns() + 2000000000ULL;
    auto wait = [&](auto complete) {
        while (!complete() && !cancelled() && clock_ns() < deadline) {
            pa_mainloop_iterate(loop, 0, nullptr);
            const auto state = pa_context_get_state(context);
            if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return complete() && !cancelled();
    };
    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0 ||
        !wait([&] { return pa_context_get_state(context) == PA_CONTEXT_READY; })) {
        cleanup(); throw std::runtime_error("PulseAudio unavailable (PipeWire requires pipewire-pulse)");
    }
    auto *operation = pa_context_get_server_info(context, [](pa_context *, const pa_server_info *info, void *data) {
        auto &q = *static_cast<Query *>(data);
        if (info) { q.source = QString::fromUtf8(info->default_source_name); q.sink = QString::fromUtf8(info->default_sink_name); }
        q.server = true;
    }, &query);
    bool ok = operation && wait([&] { return query.server; });
    if (operation) { pa_operation_cancel(operation); pa_operation_unref(operation); }
    if (ok && kind == AudioSourceKind::SystemOutput) {
        const auto sink = query.sink.toUtf8(); query.source.clear();
        operation = sink.isEmpty() ? nullptr : pa_context_get_sink_info_by_name(context, sink.constData(),
            [](pa_context *, const pa_sink_info *info, int eol, void *data) {
                auto &q = *static_cast<Query *>(data);
                if (info && info->monitor_source != PA_INVALID_INDEX && info->monitor_source_name)
                    q.source = QString::fromUtf8(info->monitor_source_name);
                if (eol) q.done = true;
            }, &query);
        ok = operation && wait([&] { return query.done; });
        if (operation) { pa_operation_cancel(operation); pa_operation_unref(operation); }
    }
    if (ok && !query.source.isEmpty()) {
        const auto source = query.source.toUtf8(); query.done = false;
        operation = pa_context_get_source_info_by_name(context, source.constData(),
            [](pa_context *, const pa_source_info *info, int eol, void *data) {
                auto &q = *static_cast<Query *>(data);
                if (info && pa_sample_spec_valid(&info->sample_spec)) q.rate = info->sample_spec.rate;
                if (eol) q.done = true;
            }, &query);
        ok = operation && wait([&] { return query.done; });
        if (operation) { pa_operation_cancel(operation); pa_operation_unref(operation); }
    }
    cleanup();
    if (!ok || query.source.isEmpty() || !query.rate)
        throw std::runtime_error("Default audio device or monitor source unavailable");
    return {query.source, query.rate};
}
#endif

class MiniaudioAudioSource final : public IAudioSource {
    AudioSourceKind _kind;
    AudioHandler _audio;
    StatusHandler _status;
    LifecycleHandler _lifecycle;
    std::thread _worker;
    std::atomic<bool> _cancel{true}, _running{false}, _reopen{false};
    std::mutex _wait_mutex;
    std::condition_variable _wake;
    mutable std::mutex _diagnostic_mutex;
    AudioSourceDiagnostics _diagnostics;
    CaptureBuffer _buffer;
    std::atomic<uint64_t> _queued{0}, _dropped{0}, _discontinuities{0}, _latency{0};
    uint64_t _base = 0, _sample_clock = 0;
    static void receive(ma_device *device, void *, const void *input, ma_uint32 frames) noexcept {
        auto &self = *static_cast<MiniaudioAudioSource *>(device->pUserData);
        if (self._cancel.load(std::memory_order_relaxed)) return;
        auto timestamp = self._base + (self._sample_clock / device->sampleRate) * 1000000000ULL +
            (self._sample_clock % device->sampleRate) * 1000000000ULL / device->sampleRate;
        const auto now = clock_ns();
        const auto block_ns = uint64_t(frames) * 1000000000ULL / device->sampleRate;
        if (!self._sample_clock) {
            // Anchor at the first PCM block, excluding variable device-open latency.
            self._base = now > block_ns ? now - block_ns : 0;
            timestamp = self._base;
        }
        if (self._kind == AudioSourceKind::SystemOutput && now > timestamp + block_ns + 50000000ULL) {
            self._base += now - block_ns - timestamp;
            timestamp = now - block_ns;
            self._buffer.mark_gap();
        }
        self._sample_clock += frames;
        self._queued.fetch_add(frames, std::memory_order_relaxed);
        if (!self._buffer.push(static_cast<const float *>(input), frames, timestamp)) {
            self._queued.fetch_sub(frames, std::memory_order_relaxed);
            self._dropped.fetch_add(frames, std::memory_order_relaxed);
        }
    }
    static void notification(const ma_device_notification *event) noexcept {
        auto &self = *static_cast<MiniaudioAudioSource *>(event->pDevice->pUserData);
        if (event->type == ma_device_notification_type_rerouted || event->type == ma_device_notification_type_stopped ||
            event->type == ma_device_notification_type_interruption_began)
            self._reopen.store(true, std::memory_order_relaxed);
    }
    bool pause(std::chrono::milliseconds duration) {
        std::unique_lock lock(_wait_mutex);
        return _wake.wait_for(lock, duration, [&] { return _cancel.load(); });
    }
    void report_error(const QString &error) {
        _running = false;
        if (!_cancel && _status) _status(false, error);
        if (!_cancel && _lifecycle) _lifecycle(false, 1, error);
    }
#if defined(__linux__)
    void default_device(ma_device_id &id, QString &name, uint32_t &rate, std::stop_token stop = {}) {
        core::perf::Scope trace(core::perf::Stage::CaptureDiscovery);
        const auto source = pulse_default_source(_kind, [this,stop] { return _cancel.load() || stop.stop_requested(); });
        name = source.name; rate = source.rate;
        const auto bytes = name.toUtf8();
        if (size_t(bytes.size()) >= sizeof(id.pulse)) throw std::runtime_error("PulseAudio device identifier too long");
        std::memcpy(id.pulse, bytes.constData(), bytes.size() + 1);
    }
#endif
    void run() noexcept {
        unsigned failures = 0;
        while (!_cancel.load()) {
            if (failures && _status) _status(false, "Retrying audio device capture");
            ma_context context{}; ma_device device{};
#if defined(_WIN32)
            std::unique_ptr<WindowsDefaultDeviceWatcher> watcher;
#elif defined(__linux__)
            std::unique_ptr<DefaultDevicePoller> watcher;
#endif
            bool context_ready = false, device_ready = false;
            try {
                _reopen = false;
#if defined(_WIN32)
                const ma_backend backend = ma_backend_wasapi;
#elif defined(__linux__)
                const ma_backend backend = ma_backend_pulseaudio;
#else
                const ma_backend backend = ma_backend_null;
                throw std::runtime_error("Capture supports Windows and Linux");
#endif
                auto result = ma_context_init(&backend, 1, nullptr, &context);
                if (result != MA_SUCCESS) throw std::runtime_error(ma_result_description(result));
                context_ready = true;
                QString name;
                uint32_t native_rate = 0;
                ma_device_id id{};
#if defined(__linux__)
                default_device(id, name, native_rate);
#elif defined(_WIN32)
                watcher = std::make_unique<WindowsDefaultDeviceWatcher>(_kind == AudioSourceKind::SystemOutput, _reopen);
                const auto &endpoint = watcher->device_id();
                if (endpoint.size() >= sizeof(id.wasapi) / sizeof(id.wasapi[0]))
                    throw std::runtime_error("Windows endpoint identifier too long");
                std::memcpy(id.wasapi, endpoint.c_str(), (endpoint.size() + 1) * sizeof(wchar_t));
#endif
                auto type = ma_device_type_capture;
#if defined(_WIN32)
                if (_kind == AudioSourceKind::SystemOutput) type = ma_device_type_loopback;
#endif
                auto config = ma_device_config_init(type);
                config.capture.pDeviceID = &id;
#if defined(_WIN32)
                config.wasapi.noAutoStreamRouting = MA_TRUE;
#endif
                config.capture.format = ma_format_f32;
                config.capture.channels = 2; config.sampleRate = native_rate;
                config.periodSizeInMilliseconds = 10; config.periods = 2;
                config.capture.channelMixMode = ma_channel_mix_mode_default;
                config.dataCallback = receive; config.notificationCallback = notification; config.pUserData = this;
                result = ma_device_init(&context, &config, &device);
                if (result != MA_SUCCESS) throw std::runtime_error(ma_result_description(result));
                device_ready = true;
#if defined(_WIN32)
                name = QString::fromUtf8(device.capture.name);
#endif
                if (!device.sampleRate) throw std::runtime_error("Audio device returned zero sample rate");
                _buffer.reset(device.sampleRate); _queued = 0;
                const auto capture_start = clock_ns();
                _base = capture_start; _sample_clock = 0;
                { std::lock_guard lock(_diagnostic_mutex);
                    _diagnostics.backend = QString::fromUtf8(ma_get_backend_name(backend));
                    _diagnostics.device = name; _diagnostics.sample_rate = device.sampleRate;
                    _diagnostics.capacity_frames = device.sampleRate / 4;
                }
                if (_cancel) throw std::runtime_error("Capture cancelled");
                result = ma_device_start(&device);
                if (result != MA_SUCCESS) throw std::runtime_error(ma_result_description(result));
                _running = true; failures = 0;
                if (_status) _status(true, {});
                if (_lifecycle) _lifecycle(true, 0, {});
                uint64_t last_delivery = clock_ns(), audio_end = 0, timestamp_shift = 0;
#if defined(__linux__)
                watcher = std::make_unique<DefaultDevicePoller>(_reopen,
                    [this, id, rate = device.sampleRate](std::stop_token stop) {
                        ma_device_id next{}; QString next_name; uint32_t next_rate = 0;
                        default_device(next, next_name, next_rate, stop);
                        return !ma_device_id_equal(&id, &next) || next_rate != rate;
                    });
#endif
                bool first = true, synthesized = false;
                while (!_cancel && !_reopen) {
                    AudioFrame frame;
                    while (!_cancel && _buffer.pop(frame)) {
                        _queued.fetch_sub(frame.left.size(), std::memory_order_relaxed);
                        if (synthesized || frame.discontinuity) {
                            timestamp_shift = audio_end > uint64_t(frame.timestamp_ns) ? audio_end - frame.timestamp_ns : 0;
                            frame.discontinuity = true;
                            synthesized = false;
                        }
                        frame.discontinuity = frame.discontinuity || first;
                        first = false;
                        frame.timestamp_ns = std::max(audio_end, uint64_t(frame.timestamp_ns) + timestamp_shift);
                        audio_end = uint64_t(frame.timestamp_ns) + frame.left.size() * 1000000000ULL / frame.sample_rate;
                        last_delivery = clock_ns();
                        _latency = last_delivery > uint64_t(frame.timestamp_ns) ? last_delivery - frame.timestamp_ns : 0;
                        if (frame.discontinuity) ++_discontinuities;
                        if (_audio) {
                            core::perf::Scope trace(core::perf::Stage::CaptureDispatch);
                            _audio(std::move(frame));
                        }
                    }
                    const auto now = clock_ns();
                    if (_kind == AudioSourceKind::SystemOutput && !_reopen && ma_device_is_started(&device) &&
                        now - last_delivery >= 20000000ULL) {
                        const auto frames = static_cast<uint32_t>(std::min<uint64_t>(device.sampleRate / 4,
                            (now > (audio_end ? audio_end : capture_start) ? now - (audio_end ? audio_end : capture_start) : 0) * device.sampleRate / 1000000000ULL));
                        if (!frames) { pause(std::chrono::milliseconds(1)); continue; }
                        if (!audio_end) audio_end = capture_start;
                        synthesized = true;
                        frame.sample_rate = device.sampleRate; frame.timestamp_ns = audio_end;
                        frame.discontinuity = first;
                        first = false;
                        if (frame.discontinuity) ++_discontinuities;
                        _latency = now > uint64_t(frame.timestamp_ns) ? now - frame.timestamp_ns : 0;
                        frame.left.assign(frames, 0); frame.right.assign(frames, 0);
                        audio_end += frames * 1000000000ULL / device.sampleRate; last_delivery = now;
                        if (_audio && !_cancel) {
                            core::perf::Scope trace(core::perf::Stage::CaptureDispatch);
                            _audio(std::move(frame));
                        }
                    }
                    pause(std::chrono::milliseconds(1));
                }
                if (!_cancel && _status) _status(false, "Audio device changed; reopening capture");
            } catch (const std::exception &error) { report_error(QString::fromUtf8(error.what())); }
            catch (...) { report_error("Unexpected capture failure"); }
            _running = false;
#if defined(__linux__)
            watcher.reset();
#endif
            if (device_ready) {
#if defined(__linux__)
                // Stop wakes PulseAudio's blocking mainloop before uninit joins its thread.
                ma_device_stop(&device);
#endif
                ma_device_uninit(&device);
            }
#if defined(_WIN32)
            watcher.reset();
#endif
            if (context_ready) ma_context_uninit(&context);
            _queued = 0;
            if (_cancel) break;
            const auto delay = failures == 0 ? 1 : failures == 1 ? 2 : 5;
            ++failures; pause(std::chrono::seconds(delay));
        }
    }
  public:
    explicit MiniaudioAudioSource(AudioSourceKind kind) : _kind(kind) {}
    ~MiniaudioAudioSource() override { stop(); }
    void start() override {
        stop(); _dropped = 0; _discontinuities = 0; _latency = 0;
        _cancel = false; _worker = std::thread([this] { run(); });
    }
    void stop() override {
        _cancel = true; _wake.notify_all(); if (_worker.joinable()) _worker.join(); _running = false;
    }
    bool is_running() const override { return _running.load(); }
    AudioSourceKind source() const override { return _kind; }
    AudioSourceDiagnostics diagnostics() const override {
        std::lock_guard lock(_diagnostic_mutex); auto value = _diagnostics;
        value.buffered_frames = _queued.load(); value.dropped_frames = _dropped.load();
        value.discontinuities = _discontinuities.load(); value.dispatch_latency_ns = _latency.load(); return value;
    }
    void set_audio_handler(AudioHandler handler) override { _audio = std::move(handler); }
    void set_status_handler(StatusHandler handler) override { _status = std::move(handler); }
    void set_lifecycle_handler(LifecycleHandler handler) override { _lifecycle = std::move(handler); }
};
}
AudioSourceFactory default_audio_source_factory() {
    return [](AudioSourceKind kind) -> std::unique_ptr<IAudioSource> {
        if (kind == AudioSourceKind::TestSignal) return make_test_signal_source();
        return std::make_unique<MiniaudioAudioSource>(kind);
    };
}
}
