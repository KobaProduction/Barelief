#pragma once
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QHostAddress>
#include <memory>
#include <mutex>
#include <functional>
#include <optional>
#include <QMap>
#include "pipeline/graph.h"
#include "core/runtime_state.h"
#include "analyzer/analyze/analyzer.h"
#include "analyzer/analyze/spectrogram.h"
#include "pipeline/audio_source.h"

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
    bool spectrogram = true;
    uint64_t graph_revision = 0;
    uint64_t ready_timestamp_ns = 0;
    uint64_t epoch = 0;
    std::vector<float> window_signal, window, windowed_signal;
    float frequency_step = 0;
    std::shared_ptr<const HistorySnapshot> history;
    QMap<int, TypedFrame> views;
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
    // Serialized with graph apply. Handlers execute on the producing DSP/adapter
    // thread and must enqueue bounded work. Unsubscribe waits for in-flight calls;
    // callbacks must not change subscriptions or invoke pipeline commands.
    // Synchronous borrowed ingress: all retained cache data is copied before return.
    void feed_typed(int ingress_node, const TypedFrame &frame, uint64_t expected_epoch = UINT64_MAX,
                    uint64_t expected_ingress_serial = UINT64_MAX);
    uint64_t epoch() const;
    uint64_t ingress_serial(int ingress_node) const;
    // Immediate unavailable marker, only for this ingress and its descendants.
    // Keeps independent branches and output counters alive; resume signals a gap.
    void clear_ingress(int ingress_node, uint64_t expected_epoch = UINT64_MAX);
    using TypedHandler = std::function<void(int, TypedFrame)>;
    uint64_t subscribe_typed(TypedHandler handler);
    uint64_t subscribe_typed(TypedHandler handler, std::vector<int> nodes);
    void unsubscribe_typed(uint64_t token);
    void set_typed_handler(TypedHandler handler);
    QMap<int, TypedFrame> latest_views() const;
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
    AudioSourceDiagnostics capture_diagnostics() const;
    WLEDAudioReactiveServer *wled() const;

  private:
    void command(std::function<void()> operation) const;
    void process(const SpectrumFrame &frame, int ingress_node = -1);
    void status(QString component, State state, QString error = {});
    QThread _thread;
    QObject *_runtime = nullptr;
    // User intent belongs to the command thread, independently of adapter existence.
    bool _capture_requested = false, _analyzer_requested = false;
    bool _explicit_stop = false, _graph_applied = false;
    // Command-thread scope: rebuilding physical producers leaves the live graph
    // and independent network ingress running. Explicit Stop still clears them.
    bool _reconfiguring_sources = false;
    mutable std::mutex _mutex, _history_mutex, _callback_mutex, _status_mutex;
    mutable std::recursive_mutex _evaluation_mutex;
    std::mutex _typed_callback_mutex;
    QMap<uint64_t, TypedHandler> _typed_handlers;
    QMap<uint64_t, std::vector<int>> _typed_nodes;
    uint64_t _next_typed_token = 0;
    QMap<int, TypedFrame> _views;
    uint64_t _epoch = 0;
    int _compat_history_node = 4;
    int _compat_wled_node = 8;
    int _primary_source_id = 0;
    QMap<int, uint64_t> _ingress_serials;
    bool _typed_running = true;
    void invalidate_views();
    void deliver_typed(const GraphOutput &output);
    GraphOutput evaluate_view(int ingress, const TypedFrame &frame, const GraphProgram &program);
    bool process_without_delivery(int ingress, const TypedFrame &frame);
    std::vector<float> _compat_history_row;
    std::vector<float> _history_input_storage;
    signal_contracts::Status prepare_runtime(const domain::GraphDefinition &,
                                             std::vector<dataflow_runtime::IngressSpec>,
                                             application::RuntimeLimits);
    signal_contracts::Status negotiate_ingress(int, const signal_contracts::FrameView &);
    std::vector<std::vector<float>> runtime_history(int) const;
    QMap<int,TypedFrame> _incoming_histories;
    QMap<int,bool> _history_zero_latest;
    QMap<QString, ComponentStatus> _statuses;
    uint64_t _status_revision = 0;
    std::atomic<uint64_t> _analyzer_generation{0}, _capture_generation{0};
    std::unique_ptr<Analyzer> _analyzer;
    struct LocalBranch {
        int id = -1, source = 0;
        QJsonObject parameters;
        std::unique_ptr<Analyzer> analyzer;
        std::unique_ptr<IAudioSource> capture;
        uint64_t sequence = 0;
    };
    std::vector<std::unique_ptr<LocalBranch>> _local_branches;
    std::atomic<int64_t> _accepted_after_ns{0};
    std::unique_ptr<Spectrogram> _history;
    AudioSourceFactory _source_factory;
    std::unique_ptr<IAudioSource> _capture;
    std::unique_ptr<WLEDAudioReactiveServer> _wled;
    std::shared_ptr<const GraphProgram> _graph;
    dataflow_runtime::GraphExecutor _executor;
    application::GraphLifecycle _lifecycle{_executor, _executor, domain::desktop_capabilities()};
    std::vector<dataflow_runtime::IngressSpec> _ingress_specs;
    application::RuntimeLimits _runtime_limits;
    QMap<int, uint64_t> _legacy_sequence;
    uint64_t _graph_revision = 0;
    std::atomic<bool> _history_pending{false};
    QTimer *_history_timer = nullptr;
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
    QTimer *_wled_timer = nullptr;
    std::mutex _wled_mailbox_mutex;
    std::optional<WledFrame> _pending_wled;
    std::atomic<bool> _wled_active{false};
    std::atomic<uint64_t> _network_generation{0};
    void publish_wled(const std::vector<float> &mel, float hz, float magnitude, uint64_t graph_revision);
};
} // namespace pipeline
