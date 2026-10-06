#pragma once
#include <QString>
#include <QJsonObject>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pipeline {
struct AudioFrame {
    uint64_t sequence = 0;
    int64_t timestamp_ns = 0;
    size_t sample_rate = 0;
    uint64_t generation = 0;
    bool discontinuity = false;
    std::vector<float> left, right;
};

enum class AudioSourceKind { SystemOutput = 0, Microphone = 1, TestSignal = 2 };
struct AudioSourceDiagnostics {
    QString backend, device;
    uint32_t sample_rate = 0;
    uint64_t buffered_frames = 0, capacity_frames = 0, dropped_frames = 0, discontinuities = 0;
    uint64_t dispatch_latency_ns = 0;
};

// Desktop capture port. Frames own their buffers; callbacks may run on a producer
// thread. stop() joins producers, and no callback may execute after it returns.
// start/stop and callback replacement are serialized by AudioPipeline.
class IAudioSource {
  public:
    using AudioHandler = std::function<void(AudioFrame)>;
    using StatusHandler = std::function<void(bool running, QString error)>;
    using LifecycleHandler = std::function<void(bool ready, uint64_t result, QString error)>;
    virtual ~IAudioSource() = default;
    virtual void start() = 0;
    virtual void configure(const QJsonObject &) {}
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
