#ifndef CORE_APP_GUI_PRESENTER_H
#define CORE_APP_GUI_PRESENTER_H

#include <QObject>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QString>
#include <QSet>
#include <optional>
#include <map>
#include <functional>

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
    void initialize(QJsonObject runtime_config, const QString &startup_error = {});
    void set_graph_adapter_provider(std::function<QJsonObject()> provider) { _graph_adapter_provider=std::move(provider); }
    quint64 set_node_parameters(int node, const QJsonObject &parameters);
    void select_device(const QString &uuid, const QString &host, quint16 port);
    void update_subscription();
    void set_numeric_endpoints(std::vector<pipeline::EndpointIdentity>);
    void set_editor_numeric_endpoints(std::vector<pipeline::EndpointIdentity>);
    void set_visible_views(quint32 mask);
    void set_numeric_preview_cadence_hz(int hz);
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
    QString file_protection_warning() const { return _file_protection_warning; }
    void clear_file_protection_warning();
    const std::optional<domain::NumericProcessingGraph>& applied_numeric_graph() const { return _applied_numeric; }

  public Q_SLOTS:
    void set_spectrum_channel(int channel);
    void set_spectrum_merge(int merge);
    void apply_processing_settings();
    quint64 apply_pipeline_graph(const QJsonObject &document);

    void start_wled_audio_reactive_server();

    void stop_wled_audio_reactive_server();

    void restart_wled_audio_reactive_server();

  Q_SIGNALS:
    void on_config_updated();
    void numeric_frame_ready(const pipeline::DeliveredNumericFrame&);
    void numeric_history_ready(const pipeline::DeliveredNumericHistoryRow&);
    void numeric_history_snapshot_ready(const pipeline::DeliveredNumericFrame&,quint64 ordinal,quint64 filled_count);

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

  private:
    quint64 _subscription_epoch = 0;
    int _numeric_preview_cadence_hz = 60;
    std::optional<quint32> _visible_views;
    quint32 _subscription = std::numeric_limits<quint32>::max();
    bool _remote = false;
    bool _numeric_persistence_mode = false;
    QJsonObject _remote_endpoint;
    GuiState &_state;
    std::shared_ptr<pipeline::EngineProvider> _provider;
    std::unique_ptr<pipeline::EnginePort> _client;
    std::map<QString, std::unique_ptr<pipeline::EnginePort>> _inactive_clients;
    bool _load_selected_snapshot = false;
    void bind_client();
    void bind_control_handlers(const QString& selected,quint64 generation);
    void bind_numeric_handlers(const QString& selected,quint64 generation);
    void bind_numeric_frame_handler(const QString& selected,quint64 generation);
    void bind_numeric_history_handler(const QString& selected,quint64 generation);
    void bind_numeric_snapshot_handler(const QString& selected,quint64 generation);
    void dispatch_queued_graph();
    void accept_applied_graph(const pipeline::EngineReply&);
    void accept_graph_reply(const pipeline::EngineReply&);
    void publish_component_statuses(const pipeline::EngineSnapshot&,bool incarnation_changed);
    void restore_selected_device_document();
    void switch_selected_client(const QString& uuid,const QString& host,quint16 port);
    void release_selected_client();
    void save_device_document();
    struct DraftPatch {
        int id;
        QJsonObject before, after;
    };
    struct PendingGraph {
        quint64 logical_id = 0, epoch = 0, graph_revision = 0;
        QJsonObject document;
        bool bootstrap = false;
        bool settings = false;
        std::vector<DraftPatch> patches;
    };
    QMap<quint64, PendingGraph> _pending_graphs;
    std::optional<PendingGraph> _queued_graph;
    quint64 _logical_request = 0;
    QString _local_device_id = "local";
    QString _device_id = "local", _graph_id = "main", _apply_status = "Draft";
    std::optional<pipeline::EngineDiagnostic> _last_numeric_rejection;
    quint64 dispatch_graph(PendingGraph request);
    void set_apply_status(QString status);
    QJsonObject _active_graph;
    QMap<QString, pipeline::ComponentStatus> _published_component_statuses;
    std::optional<domain::NumericProcessingGraph> _applied_numeric;
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
    QJsonObject graph_document_with_adapters(QJsonObject document) const;
    std::function<QJsonObject()> _graph_adapter_provider;
    QString _file_protection_warning;
    std::vector<pipeline::EndpointIdentity> _numeric_endpoints;
    std::vector<pipeline::EndpointIdentity> _editor_numeric_endpoints;
    quint64 _binding_generation=0;
    std::optional<quint64> _accepted_service_epoch;
    QString visible_saved_warning() const;
};
#endif
