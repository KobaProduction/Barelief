#ifndef CORE_APP_MAIN_WINDOW_H
#define CORE_APP_MAIN_WINDOW_H

#include "core_app/workspace/workspace_window.h"
#include <QMap>
#include <QPointer>
#include <memory>
#include <array>
#include <vector>

#include "core_app/gui_presenter.h"
#include "core_app/ui/settings_change_request.h"
#include "core_app/numeric_view_adapter.h"
#include "visualization/abstract/abs.h"
#include "visualization/spectrograms/spectrogram_graph.h"

class SchemeEditorWidget;
class QLabel;
namespace presentation { class UiCommandRegistry; class ProcessorDiscoveryPort; }

class MainWindow final : public WorkspaceWindow {
    Q_OBJECT
  public:
    explicit MainWindow(GuiPresenter &application, std::shared_ptr<presentation::ProcessorDiscoveryPort> discovery = {});
    ~MainWindow() override;

    enum class ConfigurationSaveState { Unsaved, Saving, Saved, Error, LoadError };
    void set_configuration_save_state(ConfigurationSaveState state, const QString &path = {}, const QString &error = {});

  signals:
    void start_wled_server_requested();
    void stop_wled_server_requested();
    void wled_server_config_changed();
    void on_config_updated();
    void settings_change_requested(ui::SettingsChangeRequest request);
    void save_configuration_as_requested(const QString &path);
    void open_configuration_requested(const QString &path);
    void default_graph_requested();

  private slots:
    void on_update_chart_visibility();

  private:
    QLabel *_configuration_save_status = nullptr;
    ConfigurationSaveState _configuration_save_state = ConfigurationSaveState::Unsaved;
    QString _configuration_save_path, _configuration_save_error;
    QTimer _camera_save_timer, _workspace_save_timer;
    QTimer _numeric_render_timer;
    bool _numeric_render_dirty=false;
    bool _numeric_render_queued=false;
    bool _numeric_snapshot_dirty=false;
    QMap<QString, WorkspaceDock *> _panels;
    QMap<QString, bool> _render_visible;
    QByteArray _default_workspace_state;
    bool _restoring_layout = true, _workspace_closed = false;
    bool _restoring_preset = false;
    std::optional<domain::NumericProcessingGraph> _view_draft;
    QString _view_draft_device;
    QJsonObject capture_preset_presentation();
    void restore_preset_presentation(const QJsonObject &presentation);
    QPointer<WorkspaceDock> _last_graph_slot;
    int _next_view_instance = 1;
    QList<WorkspaceDock *> graph_docks() const;
    bool in_editor_window(WorkspaceDock *dock) const;
    WorkspaceDock *new_graph_view(int node,std::optional<domain::PortId> port=std::nullopt);
    bool graph_for_view(int node,domain::NumericProcessingGraph &graph) const;
    void populate_graph_menu(int node, QMenu *menu);
    void rebuild_graph_areas(WorkspaceDock *dock, WorkspaceDock *anchor, int placement);
    void add_graph_node(int node, int placement);
    bool is_graph_dock(WorkspaceDock *dock) const;
    WorkspaceDock *add_panel(const QString &id, const QString &title, QWidget *view, bool UIApplicationConfig::*flag);
    void reset_workspace();
    void setup_graph_dock_controls(WorkspaceDock *dock);
    void install_graph_name_control(WorkspaceDock *dock);
    void request_graph_rename(WorkspaceDock *dock);
    void refresh_graph_title(WorkspaceDock *dock);
    void rename_graph(WorkspaceDock *dock, const QString &name);
    void set_graph_exposed(const QString &key, WorkspaceDock *dock, bool exposed);
    void place_graph(WorkspaceDock *dock, int placement);
    WorkspaceDock *graph_dock(int node) const;
    void open_graph_node(int node);
    void refresh_graph_navigation(const QMap<int,QString> &previous);
    struct NodeView {
        int node = -1;
        QString device, graph, kind;
        WorkspaceDock *dock = nullptr;
        AbstractChartView *chart = nullptr;
        SpectrogramGraph *spectrogram = nullptr;
        uint64_t scalar_sequence=0,scalar_epoch=0;
    };
    QMap<QString, NodeView> _node_views;
    QMap<int, QString> _active_node_views;
    bool _graph_navigation_initialized=false;
    void restore_node_views();
    void sync_node_views(const QJsonObject &graph);
    void consume_numeric_frame(const pipeline::DeliveredNumericFrame&);
    void consume_numeric_history(const pipeline::DeliveredNumericHistoryRow&);
    void consume_numeric_history_snapshot(const pipeline::DeliveredNumericFrame&,quint64 ordinal,quint64 filled_count);
    void render_numeric_views(bool consumeQueuedFrames=false,bool changedOnly=false);
    void request_numeric_render(const pipeline::EndpointIdentity&);
    std::vector<pipeline::EndpointIdentity> _numeric_changed_endpoints;
    void refresh_vector_delivery_queues();
    void select_numeric_plane(const QString&,std::size_t);
    struct NumericBinding {pipeline::EndpointIdentity endpoint;std::size_t plane=0;QString device;};
    QMap<QString,NumericBinding> _numeric_bindings;
    QMap<QString,QList<NumericBinding>> _metric_bindings;
    // A bounded transport jitter queue, shared by every view/plane of an
    // endpoint. Slots retain existing owning frames; they contain no history.
    struct VectorDeliveryQueue {
        pipeline::EndpointIdentity endpoint;
        std::array<pipeline::DeliveredNumericFrame,3> frames;
        std::size_t head=0,count=0,capacity=3;
        pipeline::DeliveryStamp fence;
        std::shared_ptr<const pipeline::NumericDeliveryContext> context;
        std::uint64_t producer=0,generation=0,sequence=0;
        bool seen=false;
        void clear(){for(auto& slot:frames)slot={};head=count=0;}
    };
    std::vector<VectorDeliveryQueue> _vector_delivery_queues;
    QString _vector_delivery_device;
    presentation::NumericViewAdapter _numeric_views{{64,{2000,4096,8*1024*1024,{true,32768}},{true,32768}}};
    void clear_node_views();
    QString ensure_node_view(int node, const QString &kind, const QString &device, const QString &graph,
                             bool open = false, int instance = 0,
                             const domain::ConfiguredProcessingNode *configured = nullptr);
    void save_workspace();
    void refresh_subscriptions();
    void apply_surface_settings();
    void apply_render_rate();
    int _render_fps = 60;
    void connect_surface_status(SpectrogramGraph *view,const QString &key);
    QMap<QString,QString> _surface_errors;
    void recover_offscreen_panels();
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    GuiPresenter &_application;
    presentation::UiCommandRegistry *_commands = nullptr;

    // Статические графики
    AbstractChartView *_raw_signal_chart = nullptr;
    AbstractChartView *_window_signal_chart = nullptr;
    AbstractChartView *_windowed_signal_chart = nullptr;
    AbstractChartView *_window_function_chart = nullptr;
    AbstractChartView *_amplitudes_chart = nullptr;
    AbstractChartView *_test_amplitudes_chart = nullptr;
    SpectrogramGraph *_spectrogram_view = nullptr;

    void setup_ui();
    SchemeEditorWidget *data_flow_editor() const;
    void update_chart_visibility(const UIApplicationConfig &config);
};

#endif // CORE_APP_MAIN_WINDOW_H
