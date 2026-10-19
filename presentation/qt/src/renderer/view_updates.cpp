#include "core_app/main_window.h"
#include "core_app/settings_widget.h"
#include "visualization/charts/freq.h"
#include <QLoggingCategory>
#include <QScreen>
#include <QSignalBlocker>

Q_DECLARE_LOGGING_CATEGORY(core_app_window)

namespace {
void set_panel_visibility(WorkspaceDock* dock,bool open) {
    // ADS raises an already-open dock on toggleView(true). Preserve the
    // selected tab when merely restoring an unchanged logical open state.
    if(dock->isClosed()==open)dock->toggleView(open);
}

void sync_chart_controls(QWidget* window,const UIApplicationConfig& config) {
    const std::pair<const char *, bool> controls[]{{"showRawSamples", config.show_raw_samples},
                                                   {"showWindowSignal", config.show_window_signal},
                                                   {"showWindowedSignal", config.show_windowed_signal},
                                                   {"showWindowFunction", config.show_window_function},
                                                   {"showFFT", config.show_amplitudes},
                                                   {"showMel", config.show_test_amplitudes},
                                                   {"showSpectrogram", config.show_spectrogram}};
    for (const auto &[name, visible] : controls) {
        if (auto *action = window->findChild<QAction *>("toggle_" + QString::fromLatin1(name))) {
            const QSignalBlocker blocker(action);
            action->setChecked(visible);
        }
    }
}
}

void MainWindow::update_chart_docks(const UIApplicationConfig &config) {
    if (config.show_spectrogram) {
        const auto size = screen()->availableGeometry().size();
        _spectrogram_view->initialize(QSize(size.width() / 2, size.height() / 2), size);
        install_graph_name_control(_panels.value("spectrogram"));
    }
    const bool restoring = _restoring_layout;
    _restoring_layout = true;
    set_panel_visibility(_panels["rawSamples"],config.show_raw_samples);
    set_panel_visibility(_panels["windowSignal"],config.show_window_signal);
    set_panel_visibility(_panels["windowedSignal"],config.show_windowed_signal);
    set_panel_visibility(_panels["windowFunction"],config.show_window_function);
    set_panel_visibility(_panels["fft"],config.show_amplitudes);
    set_panel_visibility(_panels["mel"],config.show_test_amplitudes);
    set_panel_visibility(_panels["spectrogram"],config.show_spectrogram);
    set_panel_visibility(_panels["dataFlow"],config.show_data_flow);
    _restoring_layout = restoring;
}

void MainWindow::update_chart_visibility(const UIApplicationConfig &config) {
    _application.update_subscription();
    update_chart_docks(config);
    sync_chart_controls(this,config);
    refresh_subscriptions();

    qCDebug(core_app_window, "Chart visibility updated");
}
