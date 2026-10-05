#ifndef CORE_APP_MAIN_WINDOW_H
#define CORE_APP_MAIN_WINDOW_H

#include <QDockWidget>
#include <QMap>

#include "core_app/gui_presenter.h"
#include "visualization/abstract/abs.h"
#include "visualization/spectrograms/spectrogram_graph.h"

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(GuiPresenter &application);
    ~MainWindow() override;

signals:
    void start_wled_server_requested();
    void stop_wled_server_requested();
    void wled_server_config_changed();
    void on_config_updated();

private slots:
    void on_update_chart_visibility();
    void on_raw_signal_ready(const std::vector<float>& data);
    void on_window_signal_ready(const std::vector<float>& data);
    void on_windowed_signal_ready(const std::vector<float>& data);
    void on_window_function_ready(const std::vector<float>& data);
    void on_amplitudes_ready(const std::vector<float>& data);
    void on_test_amplitudes_ready(const std::vector<float>& data);
    void on_spectrogram_updated(const std::vector<std::vector<float>>& data) const;
    void on_sample_rate_changed(uint32_t sample_rate);

private:
    QTimer _camera_save_timer, _workspace_save_timer;
    QMap<QString,QDockWidget *> _panels;
    QMap<QString,bool> _render_visible;
    QByteArray _default_workspace_state;
    bool _restoring_layout = true;
    QDockWidget *add_panel(const QString &id, const QString &title, QWidget *view, bool UIApplicationConfig::*flag);
    void reset_workspace();
    void save_workspace();
    void refresh_subscriptions();
    void recover_offscreen_panels();
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    GuiPresenter& _application;

    
    // Статические графики
    AbstractChartView* _raw_signal_chart = nullptr;
    AbstractChartView* _window_signal_chart = nullptr;
    AbstractChartView* _windowed_signal_chart = nullptr;
    AbstractChartView* _window_function_chart = nullptr;
    AbstractChartView* _amplitudes_chart = nullptr;
    AbstractChartView* _test_amplitudes_chart = nullptr;
    SpectrogramGraph* _spectrogram_view = nullptr;
    
    void setup_ui();
    void update_chart_visibility(const UIApplicationConfig& config);
};

#endif // CORE_APP_MAIN_WINDOW_H
