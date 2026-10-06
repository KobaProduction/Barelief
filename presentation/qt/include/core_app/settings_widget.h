#ifndef CORE_APP_SETTINGS_WIDGET_H
#define CORE_APP_SETTINGS_WIDGET_H

#include <QGroupBox>
#include <QAction>
#include <QLabel>
#include <functional>
#include <QScreen>
#include <QPointer>
#include <QMap>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>

#include "core_app/types.h"

class SettingsWidget : public QWidget {
    Q_OBJECT
  public:
    enum Section { General, Processing, Graphs, WLED, Network, Controls };
    void select_section(int index);
    explicit SettingsWidget(GuiState &context, QWidget *parent = nullptr);
    void set_display_screen(QScreen *screen);
    void set_graph_actions(const QList<QAction *> &actions);
    void edit_color_scale(const QString &identity);
    void set_graph_dimensions(const QString &identity, size_t frames, size_t bands);

  signals:
    void on_config_changed();
    void camera_controls_changed();
    void presentation_preferences_changed();
    void surface_settings_changed();
    void workspace_position_changed();
    void reset_camera_requested();
    void on_rebuild_charts();
    void start_wled_server_requested();
    void stop_wled_server_requested();
    void wled_server_config_changed();

  private:
    GuiState &_context;

    QVBoxLayout *_main_layout = nullptr;
    QGroupBox *_ui_group = nullptr;
    QVBoxLayout *_ui_layout = nullptr;
    QGroupBox *_network_group = nullptr;
    QVBoxLayout *_network_layout = nullptr;
    QGroupBox *_wled_group = nullptr;
    QVBoxLayout *_wled_layout = nullptr;

    QCheckBox *_enable_autosave_cb = nullptr;
    QCheckBox *_move_to_primary_screen_on_start_cb = nullptr;
    QCheckBox *_show_raw_samples_cb = nullptr;
    QCheckBox *_show_window_signal_cb = nullptr;
    QCheckBox *_show_windowed_signal_cb = nullptr;
    QCheckBox *_show_window_function_cb = nullptr;
    QCheckBox *_show_amplitudes_cb = nullptr;
    QCheckBox *_show_test_amplitudes_cb = nullptr;
    QCheckBox *_show_spectrogram_cb = nullptr;

    QCheckBox *_enable_wled_server_cb = nullptr;
    QComboBox *_network_adapter_combo = nullptr;
    QSpinBox *_port_spinbox = nullptr;

    void _setup_camera_controls();
    QWidget *_setup_graphs();
    std::function<void()> _refresh_graph_targets;
    std::function<void()> _refresh_screen_rate;
    std::function<void()> _refresh_dimensions;
    QPointer<QScreen> _display_screen;
    QMetaObject::Connection _screen_rate_connection;
    QMap<QString, QPair<size_t, size_t>> _graph_dimensions;
    QList<QPointer<QAction>> _graph_actions;
    QVBoxLayout *_graph_views_layout = nullptr;
    QLabel *_empty_graph_views = nullptr;
    void _setup_ui();
    void _connect_signals();
    void _load_current_config() const;
    void _on_config_change(bool force_save = false);
};

#endif // CORE_APP_SETTINGS_WIDGET_H
