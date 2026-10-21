#ifndef CORE_APP_MAIN_WINDOW_H
#define CORE_APP_MAIN_WINDOW_H

#include "core_app/workspace/workspace_window.h"
#include <QMap>
#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <memory>
#include <array>
#include <vector>
#include <functional>
#include <utility>

#include "core_app/gui_presenter.h"
#include "core_app/ui/settings_change_request.h"
#include "core_app/numeric_view_adapter.h"
#include "barelief/numeric_metrics_view.h"
#include "barelief/abstract/abs.h"
#include "barelief/spectrograms/spectrogram_graph.h"

class SchemeEditorWidget;
class SettingsWidget;
class GraphPanel;
class AudioNodeModel;
class QMenu;
class QActionGroup;
class QDialog;
class QHBoxLayout;
class QComboBox;
namespace presentation { class ProcessorDirectoryModel; }
class LineChartView;
class QLabel;
class QScreen;
namespace presentation { class UiCommandRegistry; class ProcessorDiscoveryPort; }

class MainWindow final : public WorkspaceWindow {
    Q_OBJECT
  public:
    explicit MainWindow(GuiPresenter &application, std::shared_ptr<presentation::ProcessorDiscoveryPort> discovery = {});
    ~MainWindow() override;

    enum class ConfigurationSaveState { Unsaved, Saving, Saved, Error, LoadError };
    void set_configuration_save_state(ConfigurationSaveState state, const QString &path = {}, const QString &error = {});
    void set_display_refresh_rate_provider(std::function<double(QScreen*)> provider);
    void refresh_display_cadence();
    // The dock owns the editor even when detached or closed.
    SchemeEditorWidget *data_flow_editor() const;

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
    void restore_node_views(const domain::NumericProcessingGraph* graph=nullptr);
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
    std::function<double(QScreen*)> _display_refresh_rate_provider;
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
    void setup_command_registry();
    void setup_numeric_render_timer();
    void setup_editor_commands();
    void setup_navigation_commands();
    void show_workspace_switcher();
    QHBoxLayout *setup_processor_selector(std::shared_ptr<presentation::ProcessorDiscoveryPort> discovery);
    presentation::ProcessorDirectoryModel *create_processor_directory(std::shared_ptr<presentation::ProcessorDiscoveryPort> discovery);
    QAction *setup_processor_manager(presentation::ProcessorDirectoryModel *directory);
    QHBoxLayout *setup_processor_corner(presentation::ProcessorDirectoryModel *directory,QAction *manage);
    void bind_processor_status(presentation::ProcessorDirectoryModel *directory);
    void bind_processor_selection(presentation::ProcessorDirectoryModel *directory);
    void bind_presenter_views();
    void setup_theme_selector(QHBoxLayout *device_layout);
    QComboBox *create_theme_combo();
    void setup_theme_menu(QComboBox *selector,QHBoxLayout *device_layout);
    void bind_theme_combo(QComboBox *selector,const QStringList& codes);
    void setup_render_telemetry();
    void refresh_fps();
    void refresh_static_chart_fps();
    void refresh_node_view_fps();
    void refresh_presentation_preferences();
    void bind_display_refresh();
    void bind_status_visibility();
    void setup_static_charts();
    LineChartView *make_line_panel(const char *id,const char *title,double minimum,double maximum,
                                   bool auto_gain,double gain_floor,bool UIApplicationConfig::*flag);
    void setup_spectrogram();
    void bind_spectrogram_state();
    std::pair<SettingsWidget *,QMenu *> setup_settings_dialog();
    QMenu *setup_settings_menu(QDialog *dialog,SettingsWidget *settings_widget);
    void bind_settings_signals(SettingsWidget *settings_widget);
    void bind_static_graph_controls();
    std::pair<SchemeEditorWidget *,QAction *> setup_editor_panel(SettingsWidget *settings_widget);
    QAction *setup_pipeline_panel(SchemeEditorWidget *editor,SettingsWidget *settings_widget);
    void bind_editor_graph_navigation(SchemeEditorWidget *editor);
    void setup_workspace_layout();
    bool restore_serialized_workspace(const UIApplicationConfig& saved_ui);
    void restore_workspace_dock_visibility(const QJsonObject& visibility,int layout_version);
    void sync_workspace_config_visibility();
    void restore_default_workspace_visibility(const UIApplicationConfig& saved_ui);
    void restore_default_node_visibility(const UIApplicationConfig& saved_ui);
    QWidget *make_panel_content(const QString& id,QWidget *view,WorkspaceDock *dock);
    void bind_panel_presentation(GraphPanel *panel);
    void save_graph_panel_preferences(const QString& id,const QJsonObject& prefs);
    void sync_panel_toggle_controls(const QString& id,bool open);
    void bind_panel_visibility(WorkspaceDock *dock,const QString& id,bool UIApplicationConfig::*flag);
    void panel_view_toggled(const QString& id,bool UIApplicationConfig::*flag,bool open);
    void save_panel_visibility(QJsonObject& visibility);
    void restore_default_dock_groups();
    void reset_workspace_flags();
    void save_workspace_flags(const QJsonObject& visibility);
    void clear_preset_node_views();
    void begin_preset_restore(domain::NumericProcessingGraph&& draft,const QJsonObject& presentation);
    void restore_preset_graph_preferences(const QJsonObject& presentation);
    void restore_preset_visibility_flags(const QJsonObject& presentation);
    void restore_preset_dock_layout(bool& restored);
    void restore_preset_node_visibility(bool restored);
    void restore_preset_surface();
    void restore_preset_camera(const QJsonObject& surface);
    void finish_preset_restore(bool previous_layout);
    void collect_numeric_subscription_endpoints(std::vector<pipeline::EndpointIdentity>& endpoints) const;
    void collect_metric_subscription_endpoints(std::vector<pipeline::EndpointIdentity>& endpoints) const;
    bool graph_subscription_exposed(const QString& key) const;
    void recover_floating_panel(QWidget *dock,const QList<QScreen *>& screens);
    void remember_graph_slot(QObject *watched);
    WorkspaceDock *graph_open_slot() const;
    WorkspaceDock *find_editor_graph_slot() const;
    bool focus_existing_graph(int node);
    void place_graph_in_slot(WorkspaceDock *dock,WorkspaceDock *slot);
    WorkspaceDock *create_graph_view_instance(int node,std::optional<domain::PortId> port,
        const domain::NumericProcessingGraph& graph,const domain::ConfiguredProcessingNode& found,
        const QJsonObject& document);
    void focus_or_place_graph(WorkspaceDock *dock,WorkspaceDock *slot,int node);
    void append_graph_port_action(QMenu*,int,domain::PortId,bool,int);
    void initialize_graph_caption(AudioNodeModel*,const QString&);
    void populate_graph_ports(int node,const domain::NumericProcessingGraph& graph,QMenu *menu);
    void populate_graph_focus(int node,QMenu *menu);
    void populate_graph_placements(int node,QMenu *menu);
    void populate_graph_targets(int node,QMenu *menu);
    void update_graph_exposure(const QString& key,WorkspaceDock *dock,bool exposed);
    WorkspaceDock *graph_placement_anchor(WorkspaceDock *dock) const;
    void expose_floating_graph(WorkspaceDock *dock);
    void bind_graph_dock_placement(WorkspaceDock *dock);
    void bind_graph_dock_visibility(WorkspaceDock *dock);
    bool is_current_graph_dock(WorkspaceDock *dock,const QString& key) const;
    void clear_scalar_input_binding(GraphPanel *panel);
    void bind_scalar_input_actions(GraphPanel *panel,AudioNodeModel *model);
    QList<QMetaObject::Connection> scalar_input_command_connections(GraphPanel *panel,AudioNodeModel *model);
    void bind_scalar_input_lifecycle(GraphPanel *panel,AudioNodeModel *model,QList<QMetaObject::Connection>& connections);
    void refresh_scalar_input_binding(WorkspaceDock *dock,AudioNodeModel *model);
    void bind_graph_caption(WorkspaceDock *dock,AudioNodeModel *model,const QString& title);
    QString graph_title(WorkspaceDock *dock,const QString& key,AudioNodeModel *model,QString title);
    void save_graph_display_name(const QString& key,const QString& title);
    void graph_navigation_actions(WorkspaceDock *&anchor,WorkspaceDock *&selected);
    void migrate_graph_navigation(WorkspaceDock *anchor);
    bool rename_bound_graph(WorkspaceDock *dock,const QString& key,const QString& title);
    void finish_graph_navigation(WorkspaceDock *selected,bool restoring,bool migrate,bool first,
                                 const QMap<int,QString>& previous);
    QJsonObject prepare_node_view_preferences(const QString& key,const QString& kind,int instance);
    WorkspaceDock *prepare_node_view_dock(const QString& key,const QString& kind,int node,int instance,bool existing);
    GraphPanel *create_node_view_panel(NodeView& view,const QString& key,const QString& kind,
        const QJsonObject& preferences,const domain::ConfiguredProcessingNode *configured);
    QString install_node_view_renderer(NodeView view,const QString& key,const QString& kind,
        bool open,const domain::ConfiguredProcessingNode *configured,bool existing,QJsonObject preferences);
    QString install_new_node_view(NodeView view,const QString& key,const QString& kind,
        bool open,const domain::ConfiguredProcessingNode *configured,bool existing,QJsonObject preferences);
    void bind_node_view_panel(GraphPanel *panel,const QString& key);
    void on_node_renderer_changed(GraphPanel *panel,const QString& key);
    void update_node_view_preferences(const QString& key,const QJsonObject& updates);
    void record_node_view_visibility(const QString& key);
    void initialize_node_view_surface(WorkspaceDock *dock,const QString& key);
    void place_new_node_view(WorkspaceDock *dock,const QString& key,const QString& device,
        const QString& graph,int node,const QString& kind,bool open,QJsonObject preferences);
    void restore_saved_node_view(const QString& key,const QJsonObject& preferences,
        const domain::NumericProcessingGraph& graph);
    void retire_node_view_session();
    void release_node_view_bindings();
    void update_view_draft();
    void rebuild_node_view_bindings(const domain::NumericProcessingGraph& graph,
        const domain::NumericProcessingGraph& runtime_graph,bool runtime_available);
    void complete_node_view_sync(const QMap<int,QString>& previous);
    bool admits_view_binding(const NumericBinding& binding,const domain::ProcessingPort& shown,
        const domain::NumericProcessingGraph& runtime_graph,bool runtime_available) const;
    void bind_primary_node_views(const domain::NumericProcessingGraph& graph,
        const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
        QSet<QString>& used,QMap<QString,NumericBinding>& bindings);
    void bind_primary_node_view(const domain::ConfiguredProcessingNode& node,
        const domain::NumericProcessingGraph& graph,const domain::NumericProcessingGraph& runtime_graph,
        bool runtime_available,const QMap<QString,QString>& fixed_views,
        QSet<QString>& used,QMap<QString,NumericBinding>& bindings);
    void add_primary_binding(const domain::ConfiguredProcessingNode& node,const domain::NumericProcessingGraph& graph,
        const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,const QString& key,
        const QString& kind,QMap<QString,NumericBinding>& bindings);
    void refresh_additional_node_views(const domain::NumericProcessingGraph& graph);
    void bind_additional_node_views(const domain::NumericProcessingGraph& graph,
        const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
        QMap<QString,NumericBinding>& bindings);
    void bind_additional_node_view(const QString& key,const NodeView& view,
        const domain::NumericProcessingGraph& graph,const domain::NumericProcessingGraph& runtime_graph,
        bool runtime_available,QMap<QString,NumericBinding>& bindings);
    void add_additional_binding(const QString& key,const NodeView& view,
        const domain::ConfiguredProcessingNode& node,const domain::NumericProcessingGraph& graph,
        const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
        QMap<QString,NumericBinding>& bindings);
    std::optional<NumericBinding> additional_view_binding(const QString& key,const NodeView& view,
        const domain::ConfiguredProcessingNode& node,const domain::NumericProcessingGraph& graph) const;
    void hide_unused_fixed_views(const QSet<QString>& used);
    QSet<QString> numeric_view_targets() const;
    void sync_metric_bindings(const domain::NumericProcessingGraph& graph,
        const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
        const QSet<QString>& targets,const QMap<QString,NumericBinding>& bindings);
    void configure_metric_view(const QString& key,const domain::NumericProcessingGraph& graph,
        const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
        QMap<QString,QList<NumericBinding>>& metric_bindings);
    void configure_scalar_metric_view(GraphPanel *panel,const QString& key,
        const domain::ConfiguredProcessingNode& node,const domain::NumericProcessingGraph& graph,
        const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
        QMap<QString,QList<NumericBinding>>& metric_bindings);
    void mark_unavailable_metric_inputs(GraphPanel *panel,const QString& key,
        const QList<NumericMetricsView::Input>& inputs,QMap<QString,QList<NumericBinding>>& metric_bindings);
    bool has_metric_binding(const NumericBinding& candidate,
        const QMap<QString,QList<NumericBinding>>& bindings) const;
    void release_old_metric_bindings(const QMap<QString,QList<NumericBinding>>& metric_bindings,
        const QMap<QString,NumericBinding>& bindings);
    void publish_metric_bindings();
    void clear_unbound_view_targets(const QSet<QString>& targets,
        const QMap<QString,NumericBinding>& bindings);
    void clear_unbound_view_target(const QString& key);
    void sync_numeric_bindings(const domain::NumericProcessingGraph& graph,
        const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
        QMap<QString,NumericBinding> bindings);
    void configure_numeric_binding(const QString& key,const NumericBinding& binding,
        const domain::NumericProcessingGraph& graph,
        const domain::NumericProcessingGraph& runtime_graph,bool runtime_available);
    void configure_frequency_grid(GraphPanel *panel,const domain::ConfiguredProcessingNode& node,
        const domain::NumericProcessingGraph& runtime_graph,bool applied);
    void configure_numeric_panel(GraphPanel *panel,const QString& key,const NumericBinding& binding,
        const domain::ConfiguredProcessingNode& node);
    void append_vector_delivery_queue(std::vector<VectorDeliveryQueue>& prepared,
        const QString& key,const NumericBinding& binding,std::size_t capacity);
    std::vector<pipeline::EndpointIdentity> selected_vector_endpoints() const;
    void restore_vector_queue(std::vector<VectorDeliveryQueue>& prepared,const NumericBinding& binding,std::size_t capacity);
    void queue_numeric_frame(VectorDeliveryQueue& queue,const pipeline::DeliveredNumericFrame& frame);
    void queue_unavailable_frame(VectorDeliveryQueue& queue,const pipeline::DeliveredNumericFrame& frame);
    void enqueue_vector_frame(VectorDeliveryQueue& queue,const pipeline::DeliveredNumericFrame& frame);
    void update_vector_queue_stamp(VectorDeliveryQueue& queue,const pipeline::DeliveredNumericFrame& frame);
    void consume_vector_delivery_queue(VectorDeliveryQueue& queue);
    void render_bound_views(const std::vector<pipeline::EndpointIdentity>& changed,bool changed_only);
    void render_metric_views(const std::vector<pipeline::EndpointIdentity>& changed,bool changed_only);
    void render_metric_view(const QString& key,const QList<NumericBinding>& bindings,
        const std::vector<pipeline::EndpointIdentity>& changed,bool changed_only);
    void consume_queued_vector_frames(std::vector<pipeline::EndpointIdentity>& changed);
    void update_graph_panel_render_rates(const QJsonObject& prefs,
        QHash<QScreen*,double>& refresh_rates,int precision);
    void update_graph_surface_settings();
    double display_refresh_rate(QScreen *display,SettingsWidget *settings,
        QHash<QScreen*,double>& cache) const;
    void update_chart_render_rates(int precision);
    void update_node_chart_render_rates(int precision);
    QJsonObject spectrogram_preferences() const;
    QString surface_identity(const QString& key) const;
    void save_surface_override(const QString& key,const QJsonObject& options);
    void show_surface_error(const QString& key,SpectrogramGraph *view,const QString& error);
    struct FileMenuActions { QAction *save; QAction *save_as; QAction *open; };
    FileMenuActions setup_file_menu();
    QMenu *setup_menu_bar_status();
    QAction *setup_save_action(QMenu *file_menu);
    QAction *setup_save_as_action(QMenu *file_menu);
    QAction *setup_open_action(QMenu *file_menu);
    void setup_view_menu(QMenu *settings_menu,SchemeEditorWidget *editor,QAction *pipeline_action,
                         const FileMenuActions& file_actions);
    QAction *setup_reset_layout_action(QMenu *view_menu);
    QAction *setup_reset_camera_action(QMenu *view_menu);
    void reset_active_surface_camera();
    void register_workspace_menu_actions(const FileMenuActions& file_actions,QAction *reset,QAction *reset_camera);
    void register_editor_menu_actions(SchemeEditorWidget *editor);
    void update_chart_visibility(const UIApplicationConfig &config);
    void update_chart_docks(const UIApplicationConfig &config);
    void register_navigation_tab_actions(QMenu*);
    void focus_data_flow();
    void detach_active_tab();
    void hide_active_panel();
    void restore_default_graph_docks();
    void focus_graph_dock(WorkspaceDock*);
    void update_render_precision(const QJsonObject&,QHash<QScreen*,double>&);
    void toggle_data_flow();
    void reset_numeric_view_endpoints();
    void prepare_numeric_render();
    void register_theme_actions(QMenu*,QComboBox*,QActionGroup*);
    void sync_theme_combo(QComboBox*,const QStringList&);
    void refresh_spectrogram_fps();
    void bind_workspace_tracking();
    void disconnect_panel_callbacks();
    void update_camera_controls_from_settings();
    void bind_workspace_position_changes(SettingsWidget*);
    bool restore_workspace_state(const QJsonObject&,int);
    void setup_workspace_save_timer();
    void add_editor_view_actions(QMenu*,SchemeEditorWidget*);
};

#endif // CORE_APP_MAIN_WINDOW_H
