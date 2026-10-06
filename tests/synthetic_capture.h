#pragma once
#include "pipeline/test_signal.h"
#include <utility>

namespace test_fixture {
// Retain the selected port identity while using only the existing eight-tone
// in-memory worker. No branch of this factory delegates to a physical driver.
class SyntheticCapture final : public pipeline::IAudioSource {
public:
    SyntheticCapture(pipeline::AudioSourceKind kind, bool deliver_audio)
        : _kind(kind), _deliver_audio(deliver_audio), _worker(pipeline::make_test_signal_source()) {}
    void start() override { _worker->start(); }
    void configure(const QJsonObject &parameters) override { _worker->configure(parameters); }
    void stop() override { _worker->stop(); }
    bool is_running() const override { return _worker->is_running(); }
    pipeline::AudioSourceKind source() const override { return _kind; }
    pipeline::AudioSourceDiagnostics diagnostics() const override { return _worker->diagnostics(); }
    void set_audio_handler(AudioHandler handler) override {
        _worker->set_audio_handler(_deliver_audio ? std::move(handler) : AudioHandler{});
    }
    void set_status_handler(StatusHandler handler) override { _worker->set_status_handler(std::move(handler)); }
    void set_lifecycle_handler(LifecycleHandler handler) override { _worker->set_lifecycle_handler(std::move(handler)); }
private:
    pipeline::AudioSourceKind _kind;
    bool _deliver_audio;
    std::unique_ptr<pipeline::IAudioSource> _worker;
};

inline pipeline::AudioSourceFactory synthetic_capture_factory(bool deliver_audio = true) {
    return [deliver_audio](pipeline::AudioSourceKind kind) {
        return std::make_unique<SyntheticCapture>(kind, deliver_audio);
    };
}
} // namespace test_fixture
