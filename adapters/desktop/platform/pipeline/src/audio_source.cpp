#include "pipeline/audio_source.h"
#include "pipeline/test_signal.h"
#include "pipeline/capture_buffer.h"
#include "pipeline/capture_wake.h"
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
    QString _device_id;
    uint32_t _admitted_rate=0, _admitted_channels=0;
    AudioHandler _audio;
    StatusHandler _status;
    LifecycleHandler _lifecycle;
    std::thread _worker;
    std::atomic<bool> _cancel{true}, _running{false}, _reopen{false};
    std::mutex _wait_mutex;
    std::condition_variable _wake;
    // One pending notification is enough: the consumer drains the SPSC ring.
    // Unlike a timed condition-variable poll, callback arrival wakes it at once.
    CaptureWake _data_ready;
    void notify_data() noexcept { _data_ready.notify(); }
    void wait_data() { _data_ready.wait_for(std::chrono::milliseconds(1)); }
    mutable std::mutex _diagnostic_mutex;
    AudioSourceDiagnostics _diagnostics;
    CaptureBuffer _buffer;
    std::atomic<uint64_t> _queued{0}, _dropped{0}, _discontinuities{0}, _latency{0};
    std::atomic<uint64_t> _callbacks{0}, _callback_frames{0}, _silence_frames{0};
    uint64_t _incarnation = 0;
    uint64_t _base = 0, _sample_clock = 0;
    uint64_t capture_timestamp(ma_device* device,ma_uint32 frames) noexcept {
        auto timestamp = _base + (_sample_clock / device->sampleRate) * 1000000000ULL +
            (_sample_clock % device->sampleRate) * 1000000000ULL / device->sampleRate;
        const auto now = clock_ns();
        const auto block_ns = uint64_t(frames) * 1000000000ULL / device->sampleRate;
        if (!_sample_clock) {
            // Anchor at the first PCM block, excluding variable device-open latency.
            _base = now > block_ns ? now - block_ns : 0;
            timestamp = _base;
        }
        if (_kind == AudioSourceKind::SystemOutput && now > timestamp + block_ns + 50000000ULL) {
            _base += now - block_ns - timestamp;
            timestamp = now - block_ns;
            _buffer.mark_gap();
        }
        return timestamp;
    }
    void record_capture(ma_device* device,const void* input,ma_uint32 frames) noexcept {
        if (_cancel.load(std::memory_order_relaxed)) return;
        _callbacks.fetch_add(1, std::memory_order_relaxed);
        _callback_frames.fetch_add(frames, std::memory_order_relaxed);
        if(!input)_silence_frames.fetch_add(frames, std::memory_order_relaxed);
        const auto timestamp=capture_timestamp(device,frames);
        const auto sample_position=_sample_clock;
        _sample_clock+=frames;
        _queued.fetch_add(frames,std::memory_order_relaxed);
        if (!_buffer.push(static_cast<const float *>(input), frames, timestamp, sample_position)) {
            _queued.fetch_sub(frames, std::memory_order_relaxed);
            _dropped.fetch_add(frames, std::memory_order_relaxed);
        }
        notify_data();
    }
    static void receive(ma_device *device, void *, const void *input, ma_uint32 frames) noexcept {
        core::perf::Scope callback_trace(core::perf::Stage::CaptureCallback);
        static_cast<MiniaudioAudioSource *>(device->pUserData)->record_capture(device,input,frames);
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
    void validate_device(const ma_device& device,uint32_t native_rate,uint32_t native_channels) const {
        if (!device.sampleRate || !device.capture.channels || device.capture.channels > 32 ||
            device.sampleRate > 192000 || device.capture.channels != device.capture.internalChannels ||
            device.sampleRate != device.capture.internalSampleRate ||
            (native_rate && native_rate != device.sampleRate) ||
            (native_channels && native_channels != device.capture.channels))
            throw std::runtime_error("Audio backend did not deliver an admitted native channel count/sample rate");
        if (_admitted_channels && (device.sampleRate != _admitted_rate || device.capture.channels != _admitted_channels))
            throw std::runtime_error("Native audio format changed; prepare the source again");
    }
    void read_native_format(ma_context& context,ma_device_id& id,uint32_t& rate,
                            uint32_t& channels,ma_format& encoding) const {
        ma_device_info endpoint_format{};
        const auto endpoint_type=_kind==AudioSourceKind::SystemOutput
            ? ma_device_type_playback : ma_device_type_capture;
        if(ma_context_get_device_info(&context,endpoint_type,&id,&endpoint_format)!=MA_SUCCESS)return;
        for(ma_uint32 i=0;i<endpoint_format.nativeDataFormatCount;++i) {
            const auto& format=endpoint_format.nativeDataFormats[i];
            if((format.flags&MA_DATA_FORMAT_FLAG_EXCLUSIVE_MODE) || !format.channels || !format.sampleRate)continue;
            rate=format.sampleRate;channels=format.channels;encoding=format.format;
            break;
        }
    }
    ma_device_config capture_config(ma_device_id& id) {
        auto type=ma_device_type_capture;
#if defined(_WIN32)
        if(_kind==AudioSourceKind::SystemOutput)type=ma_device_type_loopback;
#endif
        auto config=ma_device_config_init(type);
        config.capture.pDeviceID=&id;
#if defined(_WIN32)
        config.wasapi.noAutoStreamRouting=MA_TRUE;
        config.wasapi.noAutoConvertSRC=MA_TRUE;
#endif
        config.capture.format=ma_format_f32;
        // Native topology and native Fs; numeric Resampler owns rate changes.
        config.capture.channels=0;config.sampleRate=0;
        config.periodSizeInMilliseconds=10;config.periods=2;
        config.dataCallback=receive;config.notificationCallback=notification;config.pUserData=this;
        return config;
    }
    void open_capture_device(ma_context& context,ma_device& device,ma_device_id& id,
                             uint32_t native_rate,uint32_t native_channels) {
        if(_admitted_channels && native_channels && native_rate &&
           (native_channels!=_admitted_channels || native_rate!=_admitted_rate))
            throw std::runtime_error("Native audio format changed; prepare the source again");
        auto config=capture_config(id);
        const auto result=ma_device_init(&context,&config,&device);
        if(result!=MA_SUCCESS)
            throw std::runtime_error(std::string("Audio endpoint initialization: ")+ma_result_description(result));
    }
    void record_device_diagnostics(const ma_device& device,ma_backend backend,const QString& name,
                                   const ma_device_id& id,uint32_t native_rate,uint32_t native_channels,
                                   ma_format native_format) {
        std::lock_guard lock(_diagnostic_mutex);
        _diagnostics.backend = QString::fromUtf8(ma_get_backend_name(backend));
        _diagnostics.device = name; _diagnostics.sample_rate = device.sampleRate;
        _diagnostics.delivered_channels = device.capture.channels;
        _diagnostics.native_sample_rate = native_rate ? native_rate : device.capture.internalSampleRate;
        _diagnostics.native_channels = native_channels ? native_channels : device.capture.internalChannels;
        _diagnostics.backend_sample_rate = device.capture.internalSampleRate;
        _diagnostics.backend_channels = device.capture.internalChannels;
        _diagnostics.native_format = QString::fromUtf8(ma_get_format_name(native_format));
        _diagnostics.backend_format = QString::fromUtf8(ma_get_format_name(device.capture.internalFormat));
        _diagnostics.delivered_format = QStringLiteral("f32");
#if defined(_WIN32)
        _diagnostics.endpoint_id = QString::fromWCharArray(id.wasapi);
#elif defined(__linux__)
        (void)id;
        _diagnostics.endpoint_id = name;
#endif
        _diagnostics.capacity_frames = device.sampleRate / 4;
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
    struct CaptureTimeline {
        uint64_t last_delivery=0, audio_end=0, timestamp_shift=0, delivered_position=0;
        bool first=true, synthesized=false;
    };
    void begin_silence_frame(ma_device& device,uint64_t capture_start,uint64_t now,
                             uint32_t frames,CaptureTimeline& timeline,AudioFrame& frame) {
        if(!timeline.audio_end)timeline.audio_end=capture_start;
        timeline.synthesized=true;
        _silence_frames.fetch_add(frames,std::memory_order_relaxed);
        frame.sample_rate=device.sampleRate;frame.timestamp_ns=timeline.audio_end;
        frame.discontinuity=timeline.first;timeline.first=false;
        if(frame.discontinuity)++_discontinuities;
        _latency=now>uint64_t(frame.timestamp_ns) ? now-frame.timestamp_ns : 0;
    }
    void normalize_capture_frame(AudioFrame& frame,CaptureTimeline& timeline) {
        _queued.fetch_sub(frame.frame_count,std::memory_order_relaxed);
        if(timeline.synthesized || frame.discontinuity) {
            timeline.timestamp_shift=timeline.audio_end>uint64_t(frame.timestamp_ns)
                ? timeline.audio_end-frame.timestamp_ns : 0;
            frame.discontinuity=true;
            timeline.synthesized=false;
        }
        frame.discontinuity=frame.discontinuity || timeline.first;
        timeline.first=false;
        frame.timestamp_ns=std::max(timeline.audio_end,uint64_t(frame.timestamp_ns)+timeline.timestamp_shift);
        frame.generation=_incarnation;
        frame.sample_position=std::max(timeline.delivered_position,frame.sample_position);
        timeline.delivered_position=frame.sample_position+frame.frame_count;
        timeline.audio_end=uint64_t(frame.timestamp_ns)+frame.frame_count*1000000000ULL/frame.sample_rate;
        timeline.last_delivery=clock_ns();
        _latency=timeline.last_delivery>uint64_t(frame.timestamp_ns)
            ? timeline.last_delivery-frame.timestamp_ns : 0;
        if(frame.discontinuity)++_discontinuities;
    }
    void dispatch_capture_frame(AudioFrame& frame,CaptureTimeline& timeline) {
        normalize_capture_frame(frame,timeline);
        if(_audio) {
            core::perf::Scope trace(core::perf::Stage::CaptureDispatch);
            _audio(frame);
        }
    }
    void prepare_silence_frame(ma_device& device,uint64_t capture_start,uint64_t now,
                               uint32_t frames,CaptureTimeline& timeline,AudioFrame& frame) {
        begin_silence_frame(device,capture_start,now,frames,timeline,frame);
        frame.channel_count=device.capture.channels;frame.frame_count=frames;
        frame.generation=_incarnation;frame.sample_position=timeline.delivered_position;
        timeline.delivered_position+=frames;
        frame.samples.assign(size_t(frames)*frame.channel_count,0);
        timeline.audio_end+=frames*1000000000ULL/device.sampleRate;
        timeline.last_delivery=now;
    }
    bool synthesize_capture_frame(ma_device& device,uint64_t capture_start,CaptureTimeline& timeline,AudioFrame& frame) {
        const auto now=clock_ns();
        if(_kind!=AudioSourceKind::SystemOutput || _reopen || !ma_device_is_started(&device) ||
            now-timeline.last_delivery<20000000ULL)return false;
        const auto anchor=timeline.audio_end ? timeline.audio_end : capture_start;
        const auto frames=static_cast<uint32_t>(std::min<uint64_t>(device.sampleRate/4,
            (now>anchor ? now-anchor : 0)*device.sampleRate/1000000000ULL));
        if(!frames)return false;
        prepare_silence_frame(device,capture_start,now,frames,timeline,frame);
        if(_audio && !_cancel) {
            core::perf::Scope trace(core::perf::Stage::CaptureDispatch);
            _audio(frame);
        }
        return true;
    }
    void consume_capture(ma_device& device,uint64_t capture_start) {
        CaptureTimeline timeline{clock_ns()};
        AudioFrame frame;
        frame.samples.reserve(size_t(device.capture.channels)*std::max(1u,device.sampleRate/4));
        while(!_cancel && !_reopen) {
            while(!_cancel && _buffer.pop(frame))dispatch_capture_frame(frame,timeline);
            synthesize_capture_frame(device,capture_start,timeline,frame);
            wait_data();
        }
    }
    struct CaptureAttempt {
        ma_context context{};
        ma_device device{};
        ma_device_id id{};
        ma_backend backend=ma_backend_null;
        ma_format native_format=ma_format_unknown;
        QString name;
        uint32_t native_rate=0,native_channels=0;
        uint64_t capture_start=0;
        bool context_ready=false,device_ready=false;
#if defined(_WIN32)
        std::unique_ptr<WindowsDefaultDeviceWatcher> watcher;
#elif defined(__linux__)
        std::unique_ptr<DefaultDevicePoller> watcher;
#endif
    };
    void open_capture_context(CaptureAttempt& attempt) {
        _reopen=false;
#if defined(_WIN32)
        attempt.backend=ma_backend_wasapi;
#elif defined(__linux__)
        attempt.backend=ma_backend_pulseaudio;
#else
        throw std::runtime_error("Capture supports Windows and Linux");
#endif
        const auto result=ma_context_init(&attempt.backend,1,nullptr,&attempt.context);
        if(result!=MA_SUCCESS)
            throw std::runtime_error(std::string("Audio backend initialization: ")+ma_result_description(result));
        attempt.context_ready=true;
    }
    void select_capture_device(CaptureAttempt& attempt) {
#if defined(__linux__)
        default_device(attempt.id,attempt.name,attempt.native_rate);
#elif defined(_WIN32)
        attempt.watcher=std::make_unique<WindowsDefaultDeviceWatcher>(
            _kind==AudioSourceKind::SystemOutput,_reopen,_device_id.toStdWString());
        const auto& endpoint=attempt.watcher->device_id();
        if(endpoint.size()>=sizeof(attempt.id.wasapi)/sizeof(attempt.id.wasapi[0]))
            throw std::runtime_error("Windows endpoint identifier too long");
        std::memcpy(attempt.id.wasapi,endpoint.c_str(),(endpoint.size()+1)*sizeof(wchar_t));
#endif
    }
    void open_capture_endpoint(CaptureAttempt& attempt) {
        read_native_format(attempt.context,attempt.id,attempt.native_rate,
                           attempt.native_channels,attempt.native_format);
        open_capture_device(attempt.context,attempt.device,attempt.id,
                            attempt.native_rate,attempt.native_channels);
        attempt.device_ready=true;
#if defined(_WIN32)
        attempt.name=QString::fromUtf8(attempt.device.capture.name);
#endif
        validate_device(attempt.device,attempt.native_rate,attempt.native_channels);
        _buffer.reset(attempt.device.sampleRate,attempt.device.capture.channels);
        _queued=0;
        ++_incarnation;
        attempt.capture_start=clock_ns();
        _base=attempt.capture_start;_sample_clock=0;
        record_device_diagnostics(attempt.device,attempt.backend,attempt.name,attempt.id,
                                  attempt.native_rate,attempt.native_channels,attempt.native_format);
    }
    void start_capture_endpoint(CaptureAttempt& attempt,unsigned& failures) {
        if(_cancel)throw std::runtime_error("Capture cancelled");
        const auto result=ma_device_start(&attempt.device);
        if(result!=MA_SUCCESS)
            throw std::runtime_error(std::string("Audio endpoint start: ")+ma_result_description(result));
        _running=true;failures=0;
        if(_status)_status(true,{});
        if(_lifecycle)_lifecycle(true,0,{});
#if defined(__linux__)
        attempt.watcher=std::make_unique<DefaultDevicePoller>(_reopen,
            [this,id=attempt.id,rate=attempt.device.sampleRate](std::stop_token stop) {
                ma_device_id next{};QString next_name;uint32_t next_rate=0;
                default_device(next,next_name,next_rate,stop);
                return !ma_device_id_equal(&id,&next)||next_rate!=rate;
            });
#endif
    }
    void close_capture_attempt(CaptureAttempt& attempt) {
        _running=false;
#if defined(__linux__)
        attempt.watcher.reset();
#endif
        if(attempt.device_ready) {
#if defined(__linux__)
            ma_device_stop(&attempt.device);
#endif
            ma_device_uninit(&attempt.device);
        }
#if defined(_WIN32)
        attempt.watcher.reset();
#endif
        if(attempt.context_ready)ma_context_uninit(&attempt.context);
        _queued=0;
    }
    void capture_attempt(unsigned& failures) {
        CaptureAttempt attempt;
        try {
            open_capture_context(attempt);
            select_capture_device(attempt);
            open_capture_endpoint(attempt);
            start_capture_endpoint(attempt,failures);
            consume_capture(attempt.device,attempt.capture_start);
            if(!_cancel && _status)_status(false,"Audio device changed; reopening capture");
        } catch(const std::exception& error) {report_error(QString::fromUtf8(error.what()));}
          catch(...) {report_error("Unexpected capture failure");}
        close_capture_attempt(attempt);
    }
    void run() noexcept {
        unsigned failures=0;
        while(!_cancel.load()) {
            if(failures && _status)_status(false,"Retrying audio device capture");
            capture_attempt(failures);
            if(_cancel)break;
            const auto delay=failures==0 ? 1 : failures==1 ? 2 : 5;
            ++failures;
            pause(std::chrono::seconds(delay));
        }
    }
  public:
    explicit MiniaudioAudioSource(AudioSourceKind kind) : _kind(kind) {}
    ~MiniaudioAudioSource() override { stop(); }
    void configure(const QJsonObject& settings) override {
        _device_id=settings.value("device_id").toString();
        _admitted_channels=settings.value("channel_count").toInt();
        _admitted_rate=settings.value("sample_rate").toInt();
    }
    void start() override {
        stop(); _dropped = 0; _discontinuities = 0; _latency = 0;
        _callbacks = 0; _callback_frames = 0; _silence_frames = 0;
        { std::lock_guard lock(_diagnostic_mutex); _diagnostics = {}; }
        _cancel = false; _worker = std::thread([this] { run(); });
    }
    void stop() override {
        _cancel = true; _wake.notify_all(); notify_data(); if (_worker.joinable()) _worker.join(); _running = false;
    }
    bool is_running() const override { return _running.load(); }
    AudioSourceKind source() const override { return _kind; }
    AudioSourceDiagnostics diagnostics() const override {
        std::lock_guard lock(_diagnostic_mutex); auto value = _diagnostics;
        value.buffered_frames = _queued.load(); value.dropped_frames = _dropped.load();
        value.discontinuities = _discontinuities.load(); value.dispatch_latency_ns = _latency.load();
        value.callback_count = _callbacks.load(); value.callback_frames = _callback_frames.load();
        value.silence_frames = _silence_frames.load(); return value;
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
#if defined(_WIN32)
namespace {
AudioCaptureDevice capture_endpoint(ma_context& context,AudioSourceKind kind,const ma_device_info& entry) {
    AudioCaptureDevice endpoint{QString::fromWCharArray(entry.id.wasapi),QString::fromUtf8(entry.name),bool(entry.isDefault)};
    ma_device_info info{};
    const auto type=kind==AudioSourceKind::SystemOutput?ma_device_type_playback:ma_device_type_capture;
    if(ma_context_get_device_info(&context,type,&entry.id,&info)!=MA_SUCCESS)return endpoint;
    for(ma_uint32 format=0;format<info.nativeDataFormatCount;++format) {
        const auto& native=info.nativeDataFormats[format];
        if((native.flags & MA_DATA_FORMAT_FLAG_EXCLUSIVE_MODE) || !native.channels || !native.sampleRate)continue;
        endpoint.native_channels=native.channels;
        endpoint.native_sample_rate=native.sampleRate;
        break;
    }
    return endpoint;
}

std::vector<AudioCaptureDevice> collect_capture_endpoints(ma_context& context,AudioSourceKind kind,
    ma_device_info* playback,ma_uint32 playback_count,ma_device_info* capture,ma_uint32 capture_count) {
    const auto* entries=kind==AudioSourceKind::SystemOutput?playback:capture;
    const auto count=kind==AudioSourceKind::SystemOutput?playback_count:capture_count;
    std::vector<AudioCaptureDevice> devices;
    devices.reserve(count);
    for(ma_uint32 i=0;i<count;++i)devices.push_back(capture_endpoint(context,kind,entries[i]));
    return devices;
}
std::vector<AudioCaptureDevice> discover_context_devices(ma_context& context,AudioSourceKind kind,QString* error) {
    ma_device_info *playback=nullptr,*capture=nullptr;ma_uint32 playback_count=0,capture_count=0;
    const auto result=ma_context_get_devices(&context,&playback,&playback_count,&capture,&capture_count);
    if(result==MA_SUCCESS)return collect_capture_endpoints(context,kind,playback,playback_count,capture,capture_count);
    if(error)*error=QStringLiteral("Audio endpoint enumeration: ")+QString::fromUtf8(ma_result_description(result));
    return {};
}
std::vector<AudioCaptureDevice> enumerate_wasapi_capture_devices(AudioSourceKind kind,QString* error) {
    ma_context context{};const ma_backend backend=ma_backend_wasapi;
    auto result=ma_context_init(&backend,1,nullptr,&context);
    if(result!=MA_SUCCESS){if(error)*error=QStringLiteral("Audio device discovery: ")+QString::fromUtf8(ma_result_description(result));return {};}
    auto devices=discover_context_devices(context,kind,error);
    ma_context_uninit(&context);return devices;
}
}
#endif
std::vector<AudioCaptureDevice> enumerate_audio_capture_devices(AudioSourceKind kind, QString *error) {
    if(error)error->clear();
    if(kind==AudioSourceKind::TestSignal)return {};
#if defined(_WIN32)
    return enumerate_wasapi_capture_devices(kind,error);
#else
    if(error)*error=QStringLiteral("Explicit audio endpoint discovery is unavailable on this platform");
    return {};
#endif
}
}
