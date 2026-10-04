#include "pipeline/audio_source.h"
#include "analyzer/loopback/windows_loopback.h"

namespace pipeline {
namespace {
class WasapiAudioSource final : public IAudioSource {
  public:
    explicit WasapiAudioSource(AudioSourceKind kind)
        : _kind(kind), _capture(static_cast<audio::loopback::CaptureSource>(kind)) {}
    ~WasapiAudioSource() override {
        stop();
    }
    void start() override {
        _capture.start();
    }
    void stop() override {
        _capture.stop();
    }
    bool is_running() const override {
        return _capture.is_running();
    }
    AudioSourceKind source() const override {
        return _kind;
    }
    void set_audio_handler(AudioHandler handler) override {
        _capture.set_audio_handler([handler = std::move(handler)](audio::loopback::loopback_audio data) {
            if (!handler || !data.data || !data.channels || !data.sample_rate)
                return;
            AudioFrame frame;
            frame.sample_rate = data.sample_rate;
            frame.discontinuity = data.discontinuity;
            frame.left.assign(data.data, data.data + data.samples);
            const auto *right = data.data + (data.channels > 1 ? data.samples : 0);
            frame.right.assign(right, right + data.samples);
            handler(std::move(frame));
        });
    }
    void set_status_handler(StatusHandler handler) override {
        _capture.set_status_handler([handler = std::move(handler)](bool running, const std::string &error) {
            if (handler)
                handler(running, QString::fromStdString(error));
        });
    }
    void set_lifecycle_handler(LifecycleHandler handler) override {
        _capture.set_lifecycle_handler(
            [handler = std::move(handler)](bool ready, uint64_t result, const std::string &error) {
                if (handler)
                    handler(ready, result, QString::fromStdString(error));
            });
    }
    audio::loopback::WASAPILoopback *legacy_capture() {
        return &_capture;
    }

  private:
    AudioSourceKind _kind;
    audio::loopback::WASAPILoopback _capture;
};
} // namespace
AudioSourceFactory default_audio_source_factory() {
    return [](AudioSourceKind kind) { return std::make_unique<WasapiAudioSource>(kind); };
}
// Legacy diagnostic bridge; custom ports deliberately expose no WASAPI object.
audio::loopback::WASAPILoopback *legacy_wasapi_capture(IAudioSource *source) {
    auto *adapter = dynamic_cast<WasapiAudioSource *>(source);
    return adapter ? adapter->legacy_capture() : nullptr;
}
} // namespace pipeline
