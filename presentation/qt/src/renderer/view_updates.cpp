#include "core_app/main_window.h"
#include "core_app/settings_widget.h"
#include "visualization/charts/freq.h"
#include <QCheckBox>
#include <QLoggingCategory>
#include <QScreen>
#include <QSignalBlocker>

Q_DECLARE_LOGGING_CATEGORY(core_app_window)

void MainWindow::update_chart_visibility(const UIApplicationConfig &config) {
    _application.update_subscription();
    if (config.show_spectrogram) {
        const auto size = screen()->availableGeometry().size();
        _spectrogram_view->initialize(QSize(size.width() / 2, size.height() / 2), size);
        install_graph_name_control(_panels.value("spectrogram"));
    }
    const bool restoring = _restoring_layout;
    _restoring_layout = true;
    const auto apply_visibility=[&](const QString& key,bool open) {
        auto* dock=_panels[key];
        // ADS raises an already-open dock on toggleView(true). Preserve the
        // selected tab when merely restoring an unchanged logical open state.
        if(dock->isClosed()==open)dock->toggleView(open);
    };
    apply_visibility("rawSamples",config.show_raw_samples);
    apply_visibility("windowSignal",config.show_window_signal);
    apply_visibility("windowedSignal",config.show_windowed_signal);
    apply_visibility("windowFunction",config.show_window_function);
    apply_visibility("fft",config.show_amplitudes);
    apply_visibility("mel",config.show_test_amplitudes);
    apply_visibility("spectrogram",config.show_spectrogram);
    apply_visibility("dataFlow",config.show_data_flow);
    const std::pair<const char *, bool> controls[]{{"showRawSamples", config.show_raw_samples},
                                                   {"showWindowSignal", config.show_window_signal},
                                                   {"showWindowedSignal", config.show_windowed_signal},
                                                   {"showWindowFunction", config.show_window_function},
                                                   {"showFFT", config.show_amplitudes},
                                                   {"showMel", config.show_test_amplitudes},
                                                   {"showSpectrogram", config.show_spectrogram}};
    for (const auto &[name, visible] : controls) {
        if (auto *checkbox = findChild<QCheckBox *>(name)) {
            const QSignalBlocker blocker(checkbox);
            checkbox->setChecked(visible);
        }
        if (auto *action = findChild<QAction *>("toggle_" + QString::fromLatin1(name))) {
            const QSignalBlocker blocker(action);
            action->setChecked(visible);
        }
    }
    _restoring_layout = restoring;
    refresh_subscriptions();

    qCDebug(core_app_window, "Chart visibility updated");
}

void MainWindow::on_update_chart_visibility() {
    update_chart_visibility(_application.state().file_configs.ui);
}
