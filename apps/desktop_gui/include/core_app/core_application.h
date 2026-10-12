#ifndef CORE_APP_CORE_APPLICATION_H
#define CORE_APP_CORE_APPLICATION_H

#include <QtWidgets/QApplication>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QString>

#include "core_app/config_manager.h"
#include "core_app/gui_presenter.h"
#include "core_app/desktop_engine_provider.h"

Q_DECLARE_LOGGING_CATEGORY(core_app_main)

class MainWindow;
namespace desktop { struct NumericProjectDocument; class ProcessorDiscoveryAdapter; }

class CoreApplication final : public QApplication {
    Q_OBJECT

  public:
    CoreApplicationContext _ctx{};

    explicit CoreApplication(int &argc, char *argv[]);
    CoreApplication(int &argc, char *argv[], pipeline::AudioSourceFactory source_factory);

    ~CoreApplication() override;
    bool notify(QObject *receiver, QEvent *event) override;

    [[nodiscard]] int exec() const;

    GuiPresenter &presenter() {
        return *_presenter;
    }
    pipeline::NumericEngine &pipeline();

  public Q_SLOTS:
    void apply_processing_settings();
    void save_configuration_as(const QString &path);
    void open_configuration(const QString &path);

    void start_wled_audio_reactive_server();

    void stop_wled_audio_reactive_server();

    void restart_wled_audio_reactive_server();

  Q_SIGNALS:
    void on_config_updated();
    void pipeline_preview(const QMap<int, pipeline::NodePreview> &previews);
    void component_status_changed(QString component, int state, QString error);

  private:
    std::shared_ptr<DesktopEngineProvider> _engine_provider;
    std::shared_ptr<desktop::ProcessorDiscoveryAdapter> _processor_discovery;
    std::unique_ptr<GuiPresenter> _presenter;
    QString _remote_host;
    quint16 _remote_port = 8213;
    QStringList _serial_arguments;
    MainWindow *_main_window = nullptr;
    ConfigManager _config_manager;
    std::unique_ptr<desktop::NumericProjectDocument> _numeric_document;
    bool _numeric_draft_loaded = false;
    bool _saving_configuration = false;
    bool _configuration_project_dirty = false;
    bool _configuration_preferences_dirty = false;
    QString _configuration_save_error;
    QString _configuration_load_error;
    bool _opening_configuration = false;

    void _parse_command_line();
    bool _save_ui_preferences(bool force);
    bool _save_configuration(bool force, const QString &saveAsPath = {});
    void _update_configuration_save_status();
};

#endif // CORE_APP_CORE_APPLICATION_H
