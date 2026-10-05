#ifndef CORE_APP_GUI_PRESENTER_H
#define CORE_APP_GUI_PRESENTER_H

#include <QObject>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QString>
#include <optional>

#include "core_app/config_manager.h"
#include "pipeline/engine_client.h"

Q_DECLARE_LOGGING_CATEGORY(core_app_main)

class GuiPresenter final : public QObject {
    Q_OBJECT

  public:
    GuiState &state() {
        return _state;
    }
    pipeline::EngineClient &client() {
        return *_client;
    }
    const std::vector<float> &window() const {
        return _window;
    }
    float frequency_step() const {
        return _frequency_step;
    }

    explicit GuiPresenter(GuiState &state, const QString &remote_host = {}, quint16 remote_port = 8213,
                          QObject *parent = nullptr);

    ~GuiPresenter() override;

    bool is_remote() const {
        return _remote;
    }
    bool graph_request_pending() const {
        return !_pending_graphs.isEmpty();
    }
    void initialize();
    void update_subscription();
    void set_visible_views(quint32 mask);
    QString saved_graph_warning() const { return _saved_graph_warning; }

  public Q_SLOTS:
    void set_spectrum_channel(int channel);
    void set_spectrum_merge(int merge);
    void apply_processing_settings();
    quint64 apply_pipeline_graph(const QJsonObject &document);
    void start_analyzer();

    void stop_analyzer();

    void start_loopback();

    void stop_loopback();

    void start_wled_audio_reactive_server();

    void stop_wled_audio_reactive_server();

    void restart_wled_audio_reactive_server();

  Q_SIGNALS:
    void on_config_updated();
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
    GuiState &_state;
    std::unique_ptr<pipeline::EngineClient> _client;
    struct DraftPatch {
        int id;
        QJsonObject before, after;
    };
    struct PendingGraph {
        QJsonObject document;
        bool bootstrap = false;
        bool settings = false;
        std::vector<DraftPatch> patches;
    };
    QMap<quint64, PendingGraph> _pending_graphs;
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
