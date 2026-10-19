#ifndef CORE_APP_SETTINGS_WIDGET_H
#define CORE_APP_SETTINGS_WIDGET_H

#include <QGroupBox>
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
#include "core_app/ui/settings_change_request.h"
#include <vector>

class QTabWidget;
class QGridLayout;
class QPushButton;
class QSlider;
class GraphSettingsPage;
class SettingsWidget : public QWidget {
    Q_OBJECT
  public:
    enum Section { General, Processing, Graphs, WLED, Network, Controls };
    struct SectionDescriptor { Section section; const char *title_key; const char *default_shortcut; };
    static std::vector<SectionDescriptor> available_sections();
    void select_section(int index);
    explicit SettingsWidget(GuiState &context, QWidget *parent = nullptr);
    void set_display_screen(QScreen *screen);
    struct DisplayRates { QList<double> supported; double current=0; };
    using DisplayRateProvider=std::function<DisplayRates(QScreen*)>;
    using DisplayCurrentRateProvider=std::function<double(QScreen*)>;
    using DisplayModeRequester=std::function<bool(QScreen*,double)>; // Zero selects Maximum.
    void set_display_rate_provider(DisplayRateProvider);
    void set_display_current_rate_provider(DisplayCurrentRateProvider);
    void set_display_mode_requester(DisplayModeRequester);
    double display_refresh_rate() const;
    void set_execution_settings(QWidget *settings);
    void refresh_graph_preferences();
    void refresh_display_cadence();
    void invalidate_display_catalog();
    void edit_color_scale(const QString &identity);
    void set_graph_dimensions(const QString &identity, size_t frames, size_t bands);

  signals:
    void settings_change_requested(ui::SettingsChangeRequest request);
    void camera_controls_changed();
    void presentation_preferences_changed();
    void surface_settings_changed();
    void cadence_changed();
    void workspace_position_changed();
    void reset_camera_requested();
    void start_wled_server_requested();
    void stop_wled_server_requested();
    void wled_server_config_changed();

  private:
    friend class GraphSettingsPage;
    GuiState &_context;

    QVBoxLayout *_main_layout = nullptr;
    QVBoxLayout *_processing_layout = nullptr;
    QGroupBox *_ui_group = nullptr;
    QVBoxLayout *_ui_layout = nullptr;
    QGroupBox *_network_group = nullptr;
    QVBoxLayout *_network_layout = nullptr;
    QGroupBox *_wled_group = nullptr;
    QVBoxLayout *_wled_layout = nullptr;

    QCheckBox *_enable_autosave_cb = nullptr;
    QCheckBox *_move_to_primary_screen_on_start_cb = nullptr;

    QCheckBox *_enable_wled_server_cb = nullptr;
    QComboBox *_network_adapter_combo = nullptr;
    QSpinBox *_port_spinbox = nullptr;

    void _setup_camera_controls();
    void _setup_camera_speed(QVBoxLayout *layout,QGroupBox *group);
    void _bind_camera_speed(QSlider *speed,QLabel *value);
    void _refresh_camera_bindings(QGroupBox *group);
    void _setup_camera_bindings(QVBoxLayout *layout,QGroupBox *group);
    void _setup_camera_footer(QVBoxLayout *layout,QGroupBox *group,QLabel *status);
    void _add_camera_binding_row(QGridLayout *grid,QGroupBox *group,int row,
                                 const char *action,const char *title,QLabel *status);
    QPushButton *_add_camera_binding_button(QGridLayout *grid,QGroupBox *group,int row,
                                            const char *action,const char *title,int slot,QLabel *status);
    bool _camera_binding_conflicts(const char *action,int slot,const QJsonObject& binding) const;
    void _assign_camera_binding(const char *action,const char *title,int slot,QGroupBox *group,QLabel *status);
    void _clear_camera_alternate(const char *action,QGroupBox *group,QLabel *status);
    QWidget *_setup_graphs();
    std::function<void()> _refresh_graph_targets;
    std::function<void()> _refresh_screen_rate;
    std::function<void()> _refresh_dimensions;
    QPointer<QScreen> _display_screen;
    QMetaObject::Connection _screen_rate_connection;
    QMetaObject::Connection _screen_geometry_connection;
    DisplayRateProvider _display_rate_provider;
    DisplayCurrentRateProvider _display_current_rate_provider;
    DisplayRates _display_rates;
    bool _display_rates_dirty=true;
    DisplayModeRequester _display_mode_requester;
    QMap<QString, QPair<size_t, size_t>> _graph_dimensions;
    void apply_color_scale(const QString&,const QJsonObject&,const QJsonArray&);
    void setup_sections();
    void setup_adapter_sections(QTabWidget*);
    void setup_control_section(QTabWidget*);
    void setup_scroll_content();
    void setup_language();
    void setup_theme();
    void setup_fps();
    void setup_visibility_controls();
    void setup_wled_controls();
    void setup_network_controls();
    void populate_network_interfaces();
    void select_network_interface(int,int);
    void setup_network_port();
    void _setup_ui();
    void _connect_signals();
    void connect_preference_signals();
    void connect_wled_signals();
    void refresh_screen_catalog(bool geometryChanged);
    void _load_current_config() const;
    void _on_config_change(bool force_save = false);
    void _on_config_change(ui::SettingsChangeScope scope, bool force_save = false);
};

#endif // CORE_APP_SETTINGS_WIDGET_H
