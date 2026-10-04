#pragma once
#include <QObject>
#include <QThread>
#include <QHostAddress>
#include <memory>
#include <mutex>
#include <functional>
#include <QMap>
#include "pipeline/graph.h"
#include "core/latest_delivery.h"
#include "core/runtime_state.h"
#include "analyzer/analyze/analyzer.h"
#include "analyzer/analyze/spectrogram.h"
#include "pipeline/audio_source.h"
namespace audio::loopback { class WASAPILoopback; }
#include "wled/wled_audio_reactive_server.h"

namespace pipeline {
using State = core::RuntimeState;
struct ComponentStatus {
    State state = State::Stopped;
    QString error;
    uint64_t revision = 0;
};
struct HistorySnapshot {
    uint64_t graph_revision = 0;
    std::vector<std::vector<float>> values;
    uint64_t analyzer_generation = 0;
};
struct ViewFrame {
    SpectrumFrame spectrum;
    std::vector<float> fft, mel;
    QMap<int, NodePreview> previews;
    bool surface = true;
    uint64_t graph_revision = 0;
    uint64_t ready_timestamp_ns = 0;
    uint64_t epoch = 0;
    std::vector<float> samples, window, post_window;
    float frequency_step = 0;
    std::shared_ptr<const HistorySnapshot> history;
};
// Commands are serialized on a dedicated event loop; DSP/capture have their own workers.
// Stop producers before replacing callbacks or destroying the controller.
class AudioPipeline final {
  public:
    AudioPipeline();
    explicit AudioPipeline(AudioSourceFactory source_factory);
    ~AudioPipeline();
    AudioPipeline(const AudioPipeline &) = delete;
    AudioPipeline &operator=(const AudioPipeline &) = delete;
    void start_analyzer();
    void stop_analyzer();
    void start_capture();
    void stop_capture();
    void start_wled(const QHostAddress &interface_address, quint16 port,
                    const QHostAddress &destination = QHostAddress::Broadcast);
    void stop_wled();
    void feed(const AudioFrame &frame);
    bool apply_graph(const QJsonObject &document, QString &error);
    QJsonObject graph() const;
    uint64_t graph_revision() const;
    bool audio_view_enabled() const;
    void set_merge(SpectrumMerge mode);
    void set_view_subscription(uint32_t mask) {
        _view_subscription.store(mask);
    }
    void set_display_channel(int channel); // 0 left, 1 right, 2 merged
    void set_spectrum_handler(std::function<void(ViewFrame)> handler);
    void set_audio_handler(std::function<void(AudioFrame)> handler);
    void set_status_handler(std::function<void(QString, ComponentStatus)> handler);
    ComponentStatus current_status(const QString &component) const;
    uint64_t analyzer_generation() const {
        return _analyzer_generation.load();
    }
    uint64_t capture_generation() const {
        return _capture_generation.load();
    }
    std::vector<std::vector<float>> history() const;
    size_t history_count() const;
    std::shared_ptr<const HistorySnapshot> latest_history();
    uint64_t processed_frames() const;
    Analyzer *analyzer() const;
    Spectrogram *spectrogram() const;
    audio::loopback::WASAPILoopback *capture() const;
    WLEDAudioReactiveServer *wled() const;

  private:
    void command(std::function<void()> operation) const;
    void process(const SpectrumFrame &frame);
    void status(QString component, State state, QString error = {});
    QThread _thread;
    QObject *_runtime = nullptr;
    mutable std::mutex _mutex, _history_mutex, _callback_mutex, _status_mutex;
    QMap<QString, ComponentStatus> _statuses;
    uint64_t _status_revision = 0;
    std::atomic<uint64_t> _analyzer_generation{0}, _capture_generation{0};
    std::unique_ptr<Analyzer> _analyzer;
    std::unique_ptr<Spectrogram> _history;
    AudioSourceFactory _source_factory;
    std::unique_ptr<IAudioSource> _capture;
    std::unique_ptr<WLEDAudioReactiveServer> _wled;
    std::shared_ptr<const GraphProgram> _graph;
    uint64_t _graph_revision = 0;
    std::atomic<bool> _history_pending{false};
    std::atomic<uint64_t> _history_requested_ns{0};
    std::atomic<uint32_t> _view_subscription{31};
    std::atomic<std::shared_ptr<const HistorySnapshot>> _history_snapshot;
    size_t _rate = 48000;
    uint64_t _processed = 0;
    std::atomic<uint64_t> _audio_sequence{0};
    std::function<void(ViewFrame)> _spectrum_handler;
    std::function<void(AudioFrame)> _audio_handler;
    std::function<void(QString, ComponentStatus)> _status_handler;
    struct WledFrame {
        std::vector<float> mel;
        float hz, magnitude;
        uint64_t generation, graph_revision, enqueued_ns;
    };
    std::shared_ptr<LatestDelivery<WledFrame>> _wled_delivery;
    std::atomic<uint64_t> _network_generation{0};
    void publish_wled(const std::vector<float> &mel, float hz, float magnitude, uint64_t graph_revision);
};
} // namespace pipeline
