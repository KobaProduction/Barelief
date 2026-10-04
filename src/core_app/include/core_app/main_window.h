#ifndef CORE_APP_MAIN_WINDOW_H
#define CORE_APP_MAIN_WINDOW_H

#include <QtWidgets/QTabWidget>

#include "core_app/gui_presenter.h"
#include "visualization/abstract/abs.h"
#include "visualization/surfaces/surfacegraph.h"

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(GuiPresenter &application);
    ~MainWindow() override;

signals:
    void start_loopback_requested();
    void stop_loopback_requested();
    void start_analyzer_requested();
    void stop_analyzer_requested();
    void start_wled_server_requested();
    void stop_wled_server_requested();
    void wled_server_config_changed();
    void on_config_updated();

private slots:
    void on_update_chart_visibility();
    void on_raw_samples_ready(const std::vector<float>& data);
    void on_samples_ready(const std::vector<float>& data);
    void on_samples_after_window_function_ready(const std::vector<float>& data);
    void on_window_function_ready(const std::vector<float>& data);
    void on_amplitudes_ready(const std::vector<float>& data);
    void on_test_amplitudes_ready(const std::vector<float>& data);
    void on_spectrogram_updated(const std::vector<std::vector<float>>& data) const;
    void on_sample_rate_changed(uint32_t sample_rate);

private:
    QTimer _camera_save_timer;
    GuiPresenter& _application;
    QTabWidget* _tab_widget = nullptr;
    
    // Статические графики
    AbstractChartView* _raw_samples_chart = nullptr;
    AbstractChartView* _samples_chart = nullptr;
    AbstractChartView* _samples_after_window_function_chart = nullptr;
    AbstractChartView* _window_function_chart = nullptr;
    AbstractChartView* _amplitudes_chart = nullptr;
    AbstractChartView* _test_amplitudes_chart = nullptr;
    SurfaceGraph* _surface_view = nullptr;
    
    // Индексы табов
    int _raw_samples_tab_index = -1;
    int _samples_tab_index = -1;
    int _samples_after_window_function_tab_index = -1;
    int _window_function_tab_index = -1;
    int _amplitudes_tab_index = -1;
    int _test_amplitudes_tab_index = -1;
    int _surface_tab_index = -1;
    
    void setup_ui();
    void update_chart_visibility(const UIApplicationConfig& config);
};

#endif // CORE_APP_MAIN_WINDOW_H