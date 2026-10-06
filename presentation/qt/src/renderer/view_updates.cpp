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
    }
    const bool restoring = _restoring_layout;
    _restoring_layout = true;
    _panels["rawSamples"]->setVisible(config.show_raw_samples);
    _panels["windowSignal"]->setVisible(config.show_window_signal);
    _panels["windowedSignal"]->setVisible(config.show_windowed_signal);
    _panels["windowFunction"]->setVisible(config.show_window_function);
    _panels["fft"]->setVisible(config.show_amplitudes);
    _panels["mel"]->setVisible(config.show_test_amplitudes);
    _panels["spectrogram"]->setVisible(config.show_spectrogram);
    _panels["dataFlow"]->setVisible(config.show_data_flow);
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

void MainWindow::on_raw_signal_ready(const std::vector<float> &data) {
    if (_raw_signal_chart && _render_visible.value("rawSamples") && _raw_signal_chart->isVisible()) {
        _raw_signal_chart->update(data);
    }
}

void MainWindow::on_window_signal_ready(const std::vector<float> &data) {
    if (_window_signal_chart && _render_visible.value("windowSignal") && _window_signal_chart->isVisible()) {
        _window_signal_chart->update(data);
    }
}

void MainWindow::on_windowed_signal_ready(const std::vector<float> &data) {
    if (_windowed_signal_chart && _render_visible.value("windowedSignal") && _windowed_signal_chart->isVisible()) {
        _windowed_signal_chart->update(data);
    }
}

void MainWindow::on_window_function_ready(const std::vector<float> &data) {
    if (_window_function_chart && _render_visible.value("windowFunction") && _window_function_chart->isVisible()) {
        _window_function_chart->update(data);
    }
}

void MainWindow::on_amplitudes_ready(const std::vector<float> &data) {
    if (_amplitudes_chart && _render_visible.value("fft") && _amplitudes_chart->isVisible()) {
        static_cast<FreqChartView *>(_amplitudes_chart)->update_freq_step(_application.frequency_step());
        _amplitudes_chart->update(data);
    }
}

void MainWindow::on_test_amplitudes_ready(const std::vector<float> &data) {
    if (_test_amplitudes_chart && _render_visible.value("mel") && _test_amplitudes_chart->isVisible()) {
        _test_amplitudes_chart->update(data);
    }
}

void MainWindow::on_spectrogram_updated(const std::vector<std::vector<float>> &data) const {
    if (_spectrogram_view && _render_visible.value("spectrogram") && _spectrogram_view->isVisible()) {
        _spectrogram_view->update(data);
    }
}

void MainWindow::on_sample_rate_changed(uint32_t sample_rate) {
    if (_amplitudes_chart) {
        auto *freq_chart = static_cast<FreqChartView *>(_amplitudes_chart);
        freq_chart->update_freq_step(_application.frequency_step());
    }
    qCDebug(core_app_window, "Sample rate changed: %u Hz", sample_rate);
}
