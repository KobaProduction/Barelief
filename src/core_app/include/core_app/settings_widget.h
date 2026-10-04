#ifndef CORE_APP_SETTINGS_WIDGET_H
#define CORE_APP_SETTINGS_WIDGET_H

#include <QGroupBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>

#include "core_app/types.h"

class SettingsWidget : public QWidget {
    Q_OBJECT
public:
    void refresh_processing_settings();
    explicit SettingsWidget(GuiState& context, QWidget* parent = nullptr);

signals:
    void on_config_changed();
    void camera_controls_changed();
    void processing_settings_changed();
    void reset_camera_requested();
    void on_rebuild_charts();
    void start_loopback_requested();
    void stop_loopback_requested();
    void start_analyzer_requested();
    void stop_analyzer_requested();
    void start_wled_server_requested();
    void stop_wled_server_requested();
    void wled_server_config_changed();

private:
    GuiState& _context;

    QVBoxLayout* _main_layout = nullptr;
    QGroupBox* _core_group = nullptr;
    QVBoxLayout* _core_layout = nullptr;
    QGroupBox* _ui_group = nullptr;
    QVBoxLayout* _ui_layout = nullptr;
    QGroupBox* _wled_group = nullptr;
    QVBoxLayout* _wled_layout = nullptr;

    QCheckBox* _enable_loopback_cb = nullptr;
    QCheckBox* _enable_analyzer_cb = nullptr;

    QCheckBox* _enable_autosave_cb = nullptr;
    QCheckBox* _move_to_primary_screen_on_start_cb = nullptr;
    QCheckBox* _show_raw_samples_cb = nullptr;
    QCheckBox* _show_samples_cb = nullptr;
    QCheckBox* _show_samples_after_window_function_cb = nullptr;
    QCheckBox* _show_window_function_cb = nullptr;
    QCheckBox* _show_amplitudes_cb = nullptr;
    QCheckBox* _show_test_amplitudes_cb = nullptr;
    QCheckBox* _show_surface_cb = nullptr;

    QCheckBox* _enable_wled_server_cb = nullptr;
    QComboBox* _network_adapter_combo = nullptr;
    QSpinBox* _port_spinbox = nullptr;

    void _setup_camera_controls();
    void _setup_ui();
    void _connect_signals();
    void _load_current_config() const;
    void _on_config_change(bool force_save = false);
};

#endif // CORE_APP_SETTINGS_WIDGET_H
