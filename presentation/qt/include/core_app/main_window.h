#ifndef CORE_APP_MAIN_WINDOW_H
#define CORE_APP_MAIN_WINDOW_H

#include "core_app/workspace/workspace_window.h"
#include <QMap>
#include <QPointer>

#include "core_app/gui_presenter.h"
#include "visualization/abstract/abs.h"
#include "visualization/spectrograms/spectrogram_graph.h"

class SchemeEditorWidget;

class MainWindow final : public WorkspaceWindow {
    Q_OBJECT
  public:
    explicit MainWindow(GuiPresenter &application);
    ~MainWindow() override;

  signals:
    void start_wled_server_requested();
    void stop_wled_server_requested();
    void wled_server_config_changed();
    void on_config_updated();
    void save_configuration_as_requested(const QString &path);
    void open_configuration_requested(const QString &path);

  private slots:
    void on_update_chart_visibility();
    void on_raw_signal_ready(const std::vector<float> &data);
    void on_window_signal_ready(const std::vector<float> &data);
    void on_windowed_signal_ready(const std::vector<float> &data);
    void on_window_function_ready(const std::vector<float> &data);
    void on_amplitudes_ready(const std::vector<float> &data);
    void on_test_amplitudes_ready(const std::vector<float> &data);
    void on_spectrogram_updated(const std::vector<std::vector<float>> &data) const;
    void on_sample_rate_changed(uint32_t sample_rate);

  private:
    QTimer _camera_save_timer, _workspace_save_timer;
    QMap<QString, WorkspaceDock *> _panels;
    QMap<QString, bool> _render_visible;
    QByteArray _default_workspace_state;
    bool _restoring_layout = true, _workspace_closed = false;
    QPointer<WorkspaceDock> _last_graph_slot;
    int _next_view_instance = 1;
    QList<WorkspaceDock *> graph_docks() const;
    bool in_editor_window(WorkspaceDock *dock) const;
    WorkspaceDock *new_graph_view(int node);
    void populate_graph_menu(int node, QMenu *menu);
    void rebuild_graph_areas(WorkspaceDock *dock, WorkspaceDock *anchor, int placement);
    void add_graph_node(int node, int placement);
    bool is_graph_dock(WorkspaceDock *dock) const;
    WorkspaceDock *add_panel(const QString &id, const QString &title, QWidget *view, bool UIApplicationConfig::*flag);
    void reset_workspace();
    void setup_graph_dock_controls(WorkspaceDock *dock);
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
    };
    QMap<QString, NodeView> _node_views;
    QMap<int, QString> _active_node_views;
    bool _graph_navigation_initialized=false;
    void restore_node_views();
    void sync_node_views(const QJsonObject &graph);
    void consume_node_views(const QMap<int, pipeline::TypedFrame> &views);
    void clear_node_views();
    QString ensure_node_view(int node, const QString &kind, const QString &device, const QString &graph,
                             bool open = false, int instance = 0);
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
