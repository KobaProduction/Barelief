#pragma once
#include "pipeline/engine_models.h"
#include <QString>
#include <QJsonObject>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pipeline {
enum class AudioSourceKind { SystemOutput = 0, Microphone = 1, TestSignal = 2 };
struct AudioCaptureDevice {
    QString id, name; bool is_default = false;
    uint32_t native_sample_rate = 0, native_channels = 0;
};
// Cold platform discovery. Empty device_id in source settings follows the OS
// default; nonempty IDs select an explicit endpoint, independently of its name.
std::vector<AudioCaptureDevice> enumerate_audio_capture_devices(AudioSourceKind kind, QString *error = nullptr);
struct AudioSourceDiagnostics {
    QString backend, device, endpoint_id;
    QString native_format, backend_format, delivered_format;
    uint32_t sample_rate = 0, delivered_channels = 0;
    uint32_t native_sample_rate = 0, native_channels = 0;
    uint32_t backend_sample_rate = 0, backend_channels = 0;
    uint64_t callback_count = 0, callback_frames = 0, silence_frames = 0;
    uint64_t buffered_frames = 0, capacity_frames = 0, dropped_frames = 0, discontinuities = 0;
    uint64_t dispatch_latency_ns = 0;
};

// Desktop capture port. Frames own their buffers; callbacks may run on a producer
// thread. stop() joins producers, and no callback may execute after it returns.
// Owner lifecycle serializes start/stop and callback replacement.
class IAudioSource {
  public:
    // Borrowed synchronous adapter view. Async consumers must copy to a bounded owner.
    using AudioHandler = std::function<void(const AudioFrame&)>;
    using StatusHandler = std::function<void(bool running, QString error)>;
    using LifecycleHandler = std::function<void(bool ready, uint64_t result, QString error)>;
    virtual ~IAudioSource() = default;
    virtual void start() = 0;
    virtual void configure(const QJsonObject &) {}
    // Compatible control-plane update preserving producer clock and lifetime.
    // False means this source requires an explicit stopped reconfiguration.
    virtual bool reconfigure_live(const QJsonObject &) { return false; }
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
    virtual AudioSourceKind source() const = 0;
    virtual AudioSourceDiagnostics diagnostics() const { return {}; }
    virtual void set_audio_handler(AudioHandler handler) = 0;
    virtual void set_status_handler(StatusHandler handler) = 0;
    virtual void set_lifecycle_handler(LifecycleHandler handler) = 0;
};
using AudioSourceFactory = std::function<std::unique_ptr<IAudioSource>(AudioSourceKind)>;
AudioSourceFactory default_audio_source_factory();
} // namespace pipeline
