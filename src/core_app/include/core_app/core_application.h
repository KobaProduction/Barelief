#ifndef CORE_APP_CORE_APPLICATION_H
#define CORE_APP_CORE_APPLICATION_H

#include <QtWidgets/QApplication>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QString>

#include "core_app/config_manager.h"
#include "core_app/gui_presenter.h"

Q_DECLARE_LOGGING_CATEGORY(core_app_main)

class MainWindow;

class CoreApplication final : public QApplication {
    Q_OBJECT

  public:
    CoreApplicationContext _ctx{};

    explicit CoreApplication(int &argc, char *argv[]);

    ~CoreApplication() override;
    bool notify(QObject *receiver, QEvent *event) override;

    [[nodiscard]] int exec() const;

    GuiPresenter &presenter() {
        return *_presenter;
    }
    // Legacy diagnostics bridge; absent for a remote GUI.
    pipeline::AudioPipeline &pipeline();

  public Q_SLOTS:
    void apply_processing_settings();
    void save_configuration_as(const QString &path);
    void open_configuration(const QString &path);
    void start_analyzer();

    void stop_analyzer();

    void start_loopback();

    void stop_loopback();

    void start_wled_audio_reactive_server();

    void stop_wled_audio_reactive_server();

    void restart_wled_audio_reactive_server();

  Q_SIGNALS:
    void on_config_updated();
    void pipeline_preview(const QMap<int, pipeline::NodePreview> &previews);
    void component_status_changed(QString component, int state, QString error);

    void raw_signal_ready(const std::vector<float> &samples);

    void window_signal_ready(const std::vector<float> &samples);

    void windowed_signal_ready(const std::vector<float> &samples);

    void window_function_ready(const std::vector<float> &window);

    void amplitudes_ready(const std::vector<float> &amplitudes);

    void test_amplitudes_ready(const std::vector<float> &test_amplitudes);

    void spectrogram_updated(const std::vector<std::vector<float>> &spectrogram);

    void sample_rate_changed(uint32_t sample_rate);

    void analyzer_started();

    void analyzer_stopped();

    void loopback_started();

    void loopback_stopped();

  private:
    std::unique_ptr<GuiPresenter> _presenter;
    QString _remote_host;
    quint16 _remote_port = 8213;
    MainWindow *_main_window = nullptr;
    ConfigManager _config_manager;
    bool _opening_configuration = false;

    void _parse_command_line();
    bool _save_configuration(bool force, const QString &saveAsPath = {});
};

#endif // CORE_APP_CORE_APPLICATION_H
