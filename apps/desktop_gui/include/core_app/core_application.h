#ifndef CORE_APP_CORE_APPLICATION_H
#define CORE_APP_CORE_APPLICATION_H

#include <QtWidgets/QApplication>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QString>
#include <QSet>
#include <QMap>
#include <QPointer>
#include <QAbstractNativeEventFilter>
#include <desktop/display_refresh_rates.h>

#include "core_app/config_manager.h"
#include "core_app/gui_presenter.h"
#include "core_app/desktop_engine_provider.h"

Q_DECLARE_LOGGING_CATEGORY(core_app_main)

class MainWindow;
class QWindow;
class QScreen;
class QThread;
class SchemeEditorWidget;
class QMenu;
class QAction;
class QCommandLineParser;
class SettingsWidget;
class NumericEditorGraphModel;
namespace core_app { struct FrequencyProfileTarget; }
namespace dsp_kernels { struct FrequencyProfile; }
struct NumericExampleGraph;
namespace desktop { struct NumericProjectDocument; class ProcessorDiscoveryAdapter; struct NumericDocumentResult; struct NumericSaveConsent; struct NumericDraftPresentation; }

class CoreApplication final : public QApplication,public QAbstractNativeEventFilter {
    Q_OBJECT

  public:
    CoreApplicationContext _ctx{};

    explicit CoreApplication(int &argc, char *argv[]);
    CoreApplication(int &argc, char *argv[], pipeline::AudioSourceFactory source_factory);
    CoreApplication(int &argc, char *argv[], pipeline::AudioSourceFactory source_factory, desktop::DisplayModeApi display_api);

    ~CoreApplication() override;
    bool notify(QObject *receiver, QEvent *event) override;
    bool eventFilter(QObject *receiver, QEvent *event) override;
    bool nativeEventFilter(const QByteArray&,void*,qintptr*) override;

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
    bool _configuration_readable(const QString&);
    bool _append_remote_arguments(QStringList&);
    void _read_command_line(const QCommandLineParser&);
    struct SaveState;
    struct AsyncSave;
    std::unique_ptr<AsyncSave> _async_save;
    std::unique_ptr<QThread> _save_thread;
    QObject* _save_worker=nullptr;
    quint64 _save_job_id=0;
    quint64 _project_save_revision=0,_preferences_save_revision=0;
    bool _autosave_queued=false;
    void _queue_autosave();
    void _start_autosave();
    void _finish_autosave(bool wait=true);
    void _complete_autosave(quint64);
    void _flush_autosave();
    bool _capture_autosave(AsyncSave&);
    bool _capture_autosave_graph(AsyncSave&);
    void _capture_autosave_presentation(AsyncSave&,NumericEditorGraphModel&);
    void _launch_autosave(std::unique_ptr<AsyncSave>);
    void _accept_autosave(const AsyncSave&);
    void _bind_incremental_saves();
    void _bind_node_save_events(unsigned int);
    void _invalidate_save_graph();
    void _clear_save_changes();
    bool _save_graph_all=true;
    QSet<unsigned int> _save_nodes,_save_positions,_save_presentations,_save_adapters;
    quint64 _saved_applied_epoch=0,_saved_applied_revision=0;
    QString _configuration_error() const;
    bool _finish_preferences_save(bool,const QString&,bool);
    bool _persist_ui_preferences(QString& error);
    void _show_save_failure(const QString& path,const QString& error);
    bool _report_save_failure(const QString& error,const QString& path);
    bool _save_current_project(bool,const QString&);
    bool _write_saved_project(SaveState&,desktop::NumericProjectDocument&);
    bool _save_project(SaveState& state,bool force);
    bool _prepare_save_destination(SaveState& state,bool force);
    bool _readable_save_destination(const desktop::NumericSaveConsent& destination) const;
    bool _confirm_configuration_overwrite(const QString& target);
    bool _collect_save_draft(SaveState& state);
    bool _prepare_save_document(SaveState& state);
    bool _collect_save_presentation(const desktop::NumericProjectDocument& document,desktop::NumericDraftPresentation& delta,QString& error);
    void _collect_node_presentation(NumericEditorGraphModel& model,unsigned int id,const desktop::NumericDraftPresentation& before,desktop::NumericDraftPresentation& delta) const;
    QJsonObject _persisted_node_presentation(const QJsonObject& presentation) const;
    void _commit_saved_project(SaveState& state);
    struct StartupState;
    void _restore_startup_presentation(StartupState&,SchemeEditorWidget*,const desktop::NumericDraftPresentation&);
    void _show_music_analysis();
    void _show_resampler_comparison();
    void _load_default_graph();
    bool _apply_frequency_profile(const core_app::FrequencyProfileTarget&,const dsp_kernels::FrequencyProfile&,QString&);
    void _populate_example_menu(SchemeEditorWidget*);
    void _add_example_action(QMenu*,QAction*,SchemeEditorWidget*,NumericExampleGraph);
    bool _request_display_mode(QScreen*,double);
    void _bind_display_rate_provider(SettingsWidget*);
    double _current_display_rate(QScreen*) const;
    void _prepare_startup(StartupState& state);
    void _bind_window_actions(StartupState& state);
    void _initialize_display(desktop::DisplayModeApi display_api);
    void _initialize_identity();
    void _read_startup_project(StartupState& state);
    void _admit_startup_project(StartupState& state,std::unique_ptr<desktop::NumericProjectDocument> source,desktop::NumericDocumentResult result);
    void _handle_startup_failure(StartupState& state);
    void _recover_startup_workspace(StartupState& state);
    void _configure_serial_runtime(StartupState& state);
    void _create_startup_example(StartupState& state);
    void _create_editor_models();
    void _load_initial_model(StartupState& state);
    void _compose_engine(StartupState& state,pipeline::AudioSourceFactory source_factory);
    void _bind_editor_capabilities();
    QJsonArray _capture_devices(int,QString*) const;
    domain::ProcessingDiagnostic _validate_node(const domain::ConfiguredProcessingNode&) const;
    void _bind_server_events();
    void _create_main_window();
    void _bind_display_settings();
    void _bind_display_events();
    void _bind_default_graph_action();
    void _bind_analysis_tools();
    void _bind_editor_actions(StartupState& state);
    void _restore_startup_draft(StartupState& state,SchemeEditorWidget* editor);
    void _bind_configuration_events(StartupState& state);
    void _start_runtime(StartupState& state);
    void _start_heartbeat();
    void _load_example(SchemeEditorWidget*,NumericExampleGraph);
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
    desktop::DisplayModeApi _display_api;
    std::unique_ptr<desktop::DisplayRefreshSession> _display_session;
    QMap<QWindow*,QPointer<QWidget>> _display_windows;
    QSet<QString> _managed_displays;
    QSet<QString> _required_displays;
    QSet<QString> _display_reconciliations;
    bool _display_sync_queued=false;
    bool _synchronizing_displays=false;
    bool _injected_display_api=false;
    QString _display_device(QScreen *screen) const;
    void _watch_display_window(QWidget *widget);
    void _synchronize_display_modes();
    void _defer_display_sync();
    void _display_catalog_changed();
    void _finish_display_reconciliation(const QString&,int);
    QSet<QString> _required_display_devices(const QJsonObject&) const;
    void _release_display_modes(const QSet<QString>&);
    void _apply_display_modes(const QSet<QString>&,const QJsonObject&);
    void _refresh_display_cadence();
    void _reconcile_display_mode(const QString&,int attempts=3);

    void _parse_command_line();
    bool _save_ui_preferences(bool force);
    bool _save_configuration(bool force, const QString &saveAsPath = {});
    void _update_configuration_save_status();
};

#endif // CORE_APP_CORE_APPLICATION_H
