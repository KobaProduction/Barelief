#ifndef CORE_APP_GUI_PRESENTER_H
#define CORE_APP_GUI_PRESENTER_H

#include <QObject>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QString>
#include <optional>
#include <map>

#include "core_app/types.h"
#include "pipeline/engine_port.h"

Q_DECLARE_LOGGING_CATEGORY(core_app_main)

class GuiPresenter final : public QObject {
    Q_OBJECT

  public:
    GuiState &state() {
        return _state;
    }
    pipeline::EnginePort &client() {
        return *_client;
    }
    const std::vector<float> &window() const {
        return _window;
    }
    float frequency_step() const {
        return _frequency_step;
    }

    explicit GuiPresenter(GuiState &state, std::shared_ptr<pipeline::EngineProvider> provider,
                          const QString &remote_host = {}, quint16 remote_port = 8213, QObject *parent = nullptr);

    ~GuiPresenter() override;

    bool is_remote() const {
        return _remote;
    }
    QJsonObject remote_endpoint() const { return _remote_endpoint; }
    bool graph_request_pending() const {
        return !_pending_graphs.isEmpty() || _queued_graph.has_value();
    }
    void initialize();
    void initialize(QJsonObject runtime_config);
    quint64 set_node_parameters(int node, const QJsonObject &parameters);
    void select_device(const QString &uuid, const QString &host, quint16 port);
    void update_subscription();
    void set_visible_views(quint32 mask);
    QString local_device_id() const {
        return _local_device_id;
    }
    QString device_id() const {
        return _device_id;
    }
    QString graph_id() const {
        return _graph_id;
    }
    QString apply_status() const {
        return _apply_status;
    }
    QString saved_graph_warning() const {
        return _saved_graph_warning;
    }

  public Q_SLOTS:
    void set_spectrum_channel(int channel);
    void set_spectrum_merge(int merge);
    void apply_processing_settings();
    quint64 apply_pipeline_graph(const QJsonObject &document);
    quint64 apply_graph_and_start(const QJsonObject &document);
    void start_analyzer();

    void stop_analyzer();

    void start_loopback();

    void stop_loopback();

    void start_wled_audio_reactive_server();

    void stop_wled_audio_reactive_server();

    void restart_wled_audio_reactive_server();

  Q_SIGNALS:
    void explicit_stop_requested();
    void on_config_updated();
    void node_views_ready(const QMap<int, pipeline::TypedFrame> &views);
    void active_graph_changed(const QJsonObject &graph);
    void views_cleared();
    void selected_device_changed(const QJsonObject &draft, const QJsonObject &editor_view);
    void apply_status_changed(QString status);
    void processing_state_changed();
    void saved_graph_warning_changed(QString warning);
    void command_rejected(quint64 request_id, QString error);
    void graph_apply_finished(quint64 request_id, bool accepted, QString error);
    void pipeline_preview(const QMap<int, pipeline::NodePreview> &previews);
    void component_status_changed(QString component, int state, QString error);

    void raw_signal_ready(const std::vector<float> &samples);

    void window_signal_ready(const std::vector<float> &samples);

    void windowed_signal_ready(const std::vector<float> &samples);

    void window_function_ready(const std::vector<float> &window);

    void amplitudes_ready(const std::vector<float> &amplitudes);

    void test_amplitudes_ready(const std::vector<float> &test_amplitudes);

    void spectrogram_updated(const std::vector<std::vector<float>> &spectrogram);

    void sample_rate_changed(uint32_t sample_rate);

    void analyzer_started();

    void analyzer_stopped();

    void loopback_started();

    void loopback_stopped();

  private:
    quint64 _subscription_epoch = 0;
    std::optional<quint32> _visible_views;
    quint32 _subscription = std::numeric_limits<quint32>::max();
    bool _remote = false;
    QJsonObject _remote_endpoint;
    GuiState &_state;
    std::shared_ptr<pipeline::EngineProvider> _provider;
    std::unique_ptr<pipeline::EnginePort> _client;
    std::map<QString, std::unique_ptr<pipeline::EnginePort>> _inactive_clients;
    bool _load_selected_snapshot = false;
    void bind_client();
    void save_device_document();
    struct DraftPatch {
        int id;
        QJsonObject before, after;
    };
    struct PendingGraph {
        quint64 logical_id = 0, epoch = 0, graph_revision = 0;
        QJsonObject document;
        bool bootstrap = false;
        bool start_analyzer = false, start_capture = false;
        bool settings = false;
        std::vector<DraftPatch> patches;
    };
    QMap<quint64, PendingGraph> _pending_graphs;
    std::optional<PendingGraph> _queued_graph;
    quint64 _logical_request = 0;
    QString _local_device_id = "local";
    QString _device_id = "local", _graph_id = "main", _apply_status = "Draft";
    quint64 dispatch_graph(PendingGraph request);
    void set_apply_status(QString status);
    QJsonObject _active_graph;
    QString _saved_graph_warning;
    bool _saved_draft_unsupported = false;
    bool _preserve_saved_pipeline = false;
    quint64 _latest_settings_request = 0;
    std::vector<float> _window;
    float _frequency_step = 0;
    uint32_t _sample_rate = 48000;
    void _sync_processing_config();
    void accept_snapshot(const pipeline::EngineSnapshot &snapshot);
    quint64 submit_graph(PendingGraph request);
};
#endif
