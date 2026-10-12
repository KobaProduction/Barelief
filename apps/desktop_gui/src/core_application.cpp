#include "core_app/core_application.h"
#include "core_app/main_window.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/numeric_editor_model.h"
#include <desktop/numeric_project_document.h>
#include <desktop/processor_discovery_adapter.h>
#include <control/numeric_desktop_host.h>
#include <core_app/numeric_default_graph.h>
#include <QFile>
#include <QScopedValueRollback>
#include <QScreen>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <QStatusBar>
#include <QJsonArray>
#include <QCommandLineParser>
#include <QTimer>
#include "core/performance.h"
#include <QtNodes/internal/ConnectionIdUtils.hpp>
#include <QThread>
#include <QProcess>
#include <QFileInfo>
#include <QDir>
#include "visualization/ui_text.h"
#include <cstring>
#include "core_app/gui_event_profile.h"
#include "core_app/ui/settings_change_request.h"
#include "pipeline/serial_config.h"
#include <config/config_document.h>
#include <aggregator/aggregator_client.h>
#include <QPointer>
#include <QMenu>
#include <filesystem>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
void configure_builtin_stream_names(NumericEditorGraphModel* model) {
    if(!model)return;
    if(auto* selector=model->delegateModel<AudioNodeModel>(200);selector && selector->name()=="InputSelector") {
        const auto* node=selector->numeric_node();
        if(node && node->outputs.size()==2 && node->inputs.size()==6) {
            auto presentation=selector->presentation();
            presentation["stream_names"]=QJsonArray{ui::text("Generators"),ui::text("Loopback"),ui::text("Microphone")};
            selector->set_presentation(presentation);
        }
    }
}
uint64_t gui_thread_cycles() noexcept {
#ifdef Q_OS_WIN
    ULONG64 cycles = 0;
    return QueryThreadCycleTime(GetCurrentThread(), &cycles) ? cycles : 0;
#else
    return 0;
#endif
}
}

Q_LOGGING_CATEGORY(core_app_main, "core_app.main")
bool CoreApplication::notify(QObject *receiver, QEvent *event) {
    if (!core::perf::enabled.load(std::memory_order_relaxed) || QThread::currentThread() != thread())
        return QApplication::notify(receiver, event);
    const auto start = core::perf::now();
    const auto type = int(event->type());
    // Copy before dispatch: an event handler may delete its own receiver.
    std::array<char, 64> name{};
    if (receiver)
        std::strncpy(name.data(), receiver->metaObject()->className(), name.size() - 1);
    gui_event_profile::Frame frame{gui_event_profile::current};
    const bool detailed = gui_event_profile::enabled;
    const auto start_cycles = detailed ? gui_thread_cycles() : 0;
    if (detailed)
        gui_event_profile::current = &frame;
    // Restore the stack even when a handler throws. No owning TLS or heap work.
    struct Restore {
        gui_event_profile::Frame &frame;
        bool active;
        ~Restore() {
            if (active)
                gui_event_profile::current = frame.parent;
        }
    } restore{frame, detailed};
    const auto result = QApplication::notify(receiver, event);
    const auto duration = core::perf::now() - start;
    if (detailed) {
        const auto end_cycles = gui_thread_cycles();
        const auto cycles = end_cycles >= start_cycles ? end_cycles - start_cycles : 0;
        if (frame.parent) {
            frame.parent->children += duration;
            frame.parent->children_cycles += cycles;
        }
        gui_event_profile::record(name.data(), type, duration, frame.children, cycles, frame.children_cycles);
    }
    core::perf::record(core::perf::Stage::GuiEvent, start, duration);
    // This encloses Qt's top-level repaint/backing-store/composition work.
    // It is not a measurement of an individual private platform flush call.
    if (type == QEvent::UpdateRequest && std::strcmp(name.data(), "MainWindow") == 0)
        core::perf::record(core::perf::Stage::QtBackingStoreSync, start, duration);
    if (duration >= 50000000 && core::perf::slow_gui_event_count < core::perf::slow_gui_events.size())
        core::perf::slow_gui_events[core::perf::slow_gui_event_count++] = {duration, type, name};
    return result;
}
CoreApplication::CoreApplication(int &argc, char *argv[]) : CoreApplication(argc, argv, {}) {}

static int &prepare_gui_arguments(int &argc) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    return argc;
}

CoreApplication::CoreApplication(int &argc, char *argv[], pipeline::AudioSourceFactory source_factory)
    : QApplication(prepare_gui_arguments(argc), argv) {
    setApplicationName("Data Processing System Core");
    setApplicationVersion("1.0.0");
    setOrganizationName("Data Processing System");
    _parse_command_line();
    QString startup_error;
    const auto config_path=_config_manager.getConfigPath();
    const bool existing_config=QFileInfo::exists(config_path);
    QString load_error;
    domain::NumericProcessingGraph numeric_draft;
    bool loaded = false;
    if (existing_config) {
        auto source = std::make_unique<desktop::NumericProjectDocument>();
        QFile file(config_path);
        desktop::NumericDocumentResult result;
        if (!file.open(QIODevice::ReadOnly) || file.size()>16777216)
            result.error = "Cannot read bounded project source";
        else {
            const auto bytes=file.readAll();
            result=desktop::read_numeric_project(bytes,*source);
            if (result) { source->active_path=config_path; source->disk_baseline=bytes; }
        }
        std::uint32_t edition=0;
        if (result) result=desktop::read_project_graph_edition(*source,edition);
        if (result && edition==domain::ProcessingVersions{}.graph) {
            loaded=_config_manager.loadNumeric(*source,numeric_draft,&load_error) &&
                   _config_manager.readNumericSettings(*source,_ctx.file_configs,&load_error);
            _numeric_draft_loaded=loaded;
        } else if (result)
            load_error=QStringLiteral("Canonical numeric graph version %1 is required").arg(domain::ProcessingVersions{}.graph);
        else
            load_error=result.path.isEmpty()?result.error:result.path+": "+result.error;
        if (loaded) _numeric_document=std::move(source);
    }
    if (!loaded) {
        if(!existing_config && !QFileInfo::exists(config_path))
            qCInfo(core_app_main, "Using default configuration for missing file");
        else {
            startup_error=QString("Configuration was not loaded: %1: %2").arg(config_path,load_error);
            qCWarning(core_app_main).noquote()<<startup_error;
            // Preserve storage only. A failed boundary must not populate typed
            // settings or authorize a default processing graph.
            const auto failed=configuration::read(config_path);
            _ctx.file_configs.raw_document=failed.document;
        }
    }
    auto runtime_config=_ctx.file_configs.raw_document;
    QCommandLineParser runtime_options;pipeline::add_serial_options(runtime_options);
    QStringList runtime_arguments{applicationName()};runtime_arguments<<_serial_arguments;
    runtime_options.parse(runtime_arguments);
    QString serial_error;
    if(!pipeline::apply_serial_options(runtime_options,runtime_config,serial_error)) {
        qCritical().noquote()<<serial_error;std::exit(2);
    }
    const bool first_run_demo=!existing_config && !QFileInfo::exists(config_path) &&
        startup_error.isEmpty() && _remote_host.isEmpty() && !pipeline::read_serial_binding(runtime_config).configured;
    if(first_run_demo) {
        numeric_draft=numeric_startup_graph();
        auto source=std::make_unique<desktop::NumericProjectDocument>();
        const auto created=desktop::create_numeric_startup_project(numeric_draft,*source);
        if(!created)startup_error=created.error;
        else if(!_config_manager.readNumericSettings(*source,_ctx.file_configs,&load_error))startup_error=load_error;
        else {
            source->active_path=config_path;
            _numeric_document=std::move(source);
            _numeric_draft_loaded=true;
            runtime_config=_ctx.file_configs.raw_document;
        }
    }
    _ctx.delegate_model_registry = std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    _ctx.data_flow_graph = std::make_shared<NumericEditorGraphModel>(_ctx.delegate_model_registry);
    for (const auto &kind : numeric_editor::catalog()) {
        _ctx.delegate_model_registry->registerModel(
            [kind] { return std::make_unique<AudioNodeModel>(kind); }, numeric_editor::category(kind));
    }
    if (!_numeric_draft_loaded) {
        numeric_draft.graph_id="main";
        _numeric_draft_loaded=true;
    }
    {
        auto* model=static_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get());
        desktop::NumericDraftPresentation initial_presentation;
        if(_numeric_document) {
            const auto result=desktop::read_numeric_presentation(*_numeric_document,initial_presentation);
            if(!result)startup_error=result.error;
        }
        if (!model->set_numeric_graph(numeric_draft,&load_error,initial_presentation.node_adapters))
            startup_error=load_error;
        _ctx.file_configs.scene_graph=model->save();
    }

    _engine_provider = std::make_shared<DesktopEngineProvider>(std::move(source_factory));
    const auto serial=pipeline::read_serial_binding(runtime_config);
    if(serial.configured) {
        _engine_provider->set_serial_endpoint(serial.endpoint);
        if(_remote_host.isEmpty())_remote_host=serial.endpoint.port_name;
    }
    _presenter = std::make_unique<GuiPresenter>(_ctx, _engine_provider, _remote_host, _remote_port);
    connect(_presenter.get(), &GuiPresenter::on_config_updated, this, &CoreApplication::on_config_updated);
    connect(_presenter.get(), &GuiPresenter::pipeline_preview, this, &CoreApplication::pipeline_preview);
    connect(_presenter.get(), &GuiPresenter::component_status_changed, this,
            &CoreApplication::component_status_changed);
    if(auto* model=dynamic_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get())) {
        model->set_capture_device_provider([this](int kind,QString* error) {
            QJsonArray choices;
            const auto* host=dynamic_cast<pipeline::NumericDesktopHost*>(&_presenter->client());
            if(!host || !host->local_engine()){if(error)*error=QStringLiteral("Capture device discovery requires the local desktop device");return choices;}
            if(kind<0 || kind>1){if(error)*error=QStringLiteral("Invalid capture source kind");return choices;}
            for(const auto& device:pipeline::enumerate_audio_capture_devices(pipeline::AudioSourceKind(kind),error))
                choices.append(QJsonObject{{"id",device.id},{"name",device.name},{"is_default",device.is_default},
                    {"native_sample_rate",int(device.native_sample_rate)},{"native_channels",int(device.native_channels)}});
            return choices;
        });
        model->set_node_validator([this](const domain::ConfiguredProcessingNode& node) {
            // Resolve against the current adapter, never a captured device or
            // a presentation-owned copy of platform capability rules.
            auto* host=dynamic_cast<pipeline::NumericDesktopHost*>(&_presenter->client());
            return host?host->validate_numeric_node(node):domain::ProcessingDiagnostic{};
        });
    }
    _processor_discovery = std::make_shared<desktop::ProcessorDiscoveryAdapter>();
    _engine_provider->set_registry(_processor_discovery->client());
    _main_window = new MainWindow(*_presenter, _processor_discovery);
    connect(_main_window,&MainWindow::default_graph_requested,this,[this] {
        if(auto* editor=_main_window->findChild<SchemeEditorWidget*>()) {
            QString error;
            if(!editor->load_numeric_draft(numeric_default_graph(),&error))
                _main_window->statusBar()->showMessage(error,10000);
            else {
                editor->initialize_default_layout();
                emit editor->preset_presentation_requested({});
                _presenter->apply_pipeline_graph({});
            }
        }
    });
    if (auto* editor=_main_window->findChild<SchemeEditorWidget*>()) {
        if(auto* menu=_main_window->findChild<QMenu*>("pipelinePresetsMenu")) {
            for(const auto* id:{"defaultPipeline","generatorLoopbackPreset","useTestSource"})
                if(auto* old=menu->findChild<QAction*>(id))old->setVisible(false);
            auto* anchor=menu->findChild<QAction*>("loadActivePipeline");
            for(auto example:numeric_example_graphs()) {
                auto* action=new QAction(ui::text(example.title_key),menu);menu->insertAction(anchor,action);
                ui::bind(action,"text",example.title_key);
                action->setObjectName(QStringLiteral("example-")+QString::fromLatin1(example.id));
                connect(action,&QAction::triggered,this,[this,editor,graph=std::move(example.graph)] {
                    if(_presenter->graph_request_pending())return;
                    QString error;
                    if(!editor->load_numeric_draft(graph,&error)) {
                        _main_window->statusBar()->showMessage(error,10000);return;
                    }
                    configure_builtin_stream_names(dynamic_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get()));
                    editor->initialize_default_layout();
                    emit editor->preset_presentation_requested(QJsonObject{{"version",1},{"graph_views",QJsonObject{}},
                        {"graph_rendering",QJsonObject{}},{"workspace",QJsonObject{}},
                        {"show_raw_samples",true},{"show_amplitudes",true},{"show_test_amplitudes",true},
                        {"show_spectrogram",true},{"show_data_flow",true}});
                    _presenter->apply_pipeline_graph({});
                });
            }
        }
        connect(editor,&SchemeEditorWidget::generator_loopback_graph_requested,this,[this,editor] {
            QString error;
            if(!editor->load_numeric_draft(numeric_startup_graph(),&error))
                _main_window->statusBar()->showMessage(error,10000);
            else {
                editor->initialize_default_layout();
                emit editor->preset_presentation_requested(QJsonObject{
                    {"version",1},{"graph_views",QJsonObject{}},{"graph_rendering",QJsonObject{}},{"workspace",QJsonObject{}},
                    {"show_raw_samples",true},{"show_amplitudes",true},{"show_test_amplitudes",true},{"show_spectrogram",true},
                    {"show_window_signal",false},{"show_windowed_signal",false},{"show_window_function",false},{"show_data_flow",true}
                });
                _presenter->apply_pipeline_graph({});
            }
        });
        _presenter->set_graph_adapter_provider([guard=QPointer<SchemeEditorWidget>(editor)] {
            return guard?guard->graph_adapter_metadata():QJsonObject{};
        });
        if (_numeric_draft_loaded && startup_error.isEmpty()) {
            desktop::NumericDraftPresentation presentation;
            const auto result=_numeric_document
                ?desktop::read_numeric_presentation(*_numeric_document,presentation)
                :desktop::NumericDocumentResult{};
            if (!result) startup_error=result.error;
            else if (!editor->load_numeric_draft(numeric_draft,&load_error,presentation.node_adapters)) startup_error=load_error;
            else {
                if(first_run_demo)configure_builtin_stream_names(dynamic_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get()));
                for (auto it=presentation.positions.begin();it!=presentation.positions.end();++it) {
                    const auto position=it.value().toObject();
                    _ctx.data_flow_graph->setNodeData(it.key().toUInt(),QtNodes::NodeRole::Position,
                        QPointF(position.value("x").toDouble(),position.value("y").toDouble()));
                }
                for(auto it=presentation.node_presentations.begin();it!=presentation.node_presentations.end();++it)
                    if(auto* node=_ctx.data_flow_graph->delegateModel<AudioNodeModel>(it.key().toUInt()))
                        node->set_presentation(it.value().toObject());
                if(first_run_demo)editor->initialize_default_layout();
                _ctx.editor_draft_modified=false;
            }
        }
        connect(editor,&SchemeEditorWidget::numeric_draft_changed,this,[this] {
            _ctx.editor_draft_modified=true;
            _save_configuration(false);
        });
    }
    _configuration_load_error=startup_error;
    _configuration_project_dirty = !existing_config;
    _configuration_save_error = startup_error;
    _update_configuration_save_status();
    connect(_main_window, &MainWindow::on_config_updated, this, [this] { _save_configuration(true); });
    connect(_main_window, &MainWindow::settings_change_requested, this,
            [this](const ui::SettingsChangeRequest &request) {
        if (request.scope == ui::SettingsChangeScope::NodeAdapters)
            _save_configuration(request.force_save);
        else
            _save_ui_preferences(request.force_save);
    });
    connect(_presenter.get(), &GuiPresenter::on_config_updated, this, [this] { _save_configuration(false); });
    connect(_main_window, &MainWindow::save_configuration_as_requested, this, &CoreApplication::save_configuration_as);
    connect(_main_window, &MainWindow::open_configuration_requested, this, &CoreApplication::open_configuration);
    connect(_main_window, &MainWindow::start_wled_server_requested, _presenter.get(),
            &GuiPresenter::start_wled_audio_reactive_server);
    connect(_main_window, &MainWindow::stop_wled_server_requested, _presenter.get(),
            &GuiPresenter::stop_wled_audio_reactive_server);
    connect(_main_window, &MainWindow::wled_server_config_changed, _presenter.get(),
            &GuiPresenter::restart_wled_audio_reactive_server);
    _presenter->initialize(std::move(runtime_config),startup_error);
    if(startup_error.isEmpty())
        QTimer::singleShot(0,this,[this] {
            // Every valid project starts through the same transactional runtime path.
            _presenter->apply_pipeline_graph({});
        });
    auto *heartbeat = new QTimer(this);
    heartbeat->setTimerType(Qt::PreciseTimer);
    heartbeat->setInterval(17);
    connect(heartbeat, &QTimer::timeout, this, [last = uint64_t{0}]() mutable {
        if (!core::perf::enabled.load(std::memory_order_relaxed)) {
            last = 0;
            return;
        }
        const auto now = core::perf::now();
        if (last)
            core::perf::record(core::perf::Stage::GuiHeartbeat, last, now - last);
        last = now;
    });
    heartbeat->start();
}
void CoreApplication::_update_configuration_save_status() {
    if (!_main_window) return;
    using State = MainWindow::ConfigurationSaveState;
    const auto state = !_configuration_load_error.isEmpty() ? State::LoadError
        : !_configuration_save_error.isEmpty() ? State::Error
        : _saving_configuration ? State::Saving
        : (_configuration_project_dirty || _configuration_preferences_dirty || _ctx.editor_draft_modified)
            ? State::Unsaved : State::Saved;
    const auto error=_configuration_load_error.isEmpty() ? _configuration_save_error
        : _configuration_load_error+(_configuration_save_error.isEmpty() || _configuration_save_error==_configuration_load_error
            ? QString{} : "\n"+_configuration_save_error);
    _main_window->set_configuration_save_state(state, _config_manager.getConfigPath(), error);
}

bool CoreApplication::_save_ui_preferences(bool force) {
    _configuration_preferences_dirty = true;
    _update_configuration_save_status();
    if (!_main_window || _saving_configuration) return false;
    if (!force && !_ctx.file_configs.ui.autosave_enabled) return true;
    _configuration_save_error.clear();
    bool saved = false;
    QString error = _configuration_load_error;
    if (error.isEmpty() && !_numeric_document)
        error = QStringLiteral("Canonical project document is unavailable");
    {
        const QScopedValueRollback saving(_saving_configuration, true);
        _update_configuration_save_status();
        saved = error.isEmpty() && _config_manager.saveNumericPreferences(*_numeric_document, _ctx.file_configs, &error);
    }
    if (saved) {
        _configuration_preferences_dirty = false;
        _update_configuration_save_status();
        return true;
    }
    _configuration_save_error = error;
    _update_configuration_save_status();
    _main_window->statusBar()->showMessage(
        ui::text("Could not save settings: %1\n%2").arg(_config_manager.getConfigPath(), error), 10000);
    return false;
}

bool CoreApplication::_save_configuration(bool force, const QString &saveAsPath) {
    if(!_main_window)return false; // No widget-tree access after teardown starts.
    _configuration_project_dirty = true;
    _update_configuration_save_status();
    if (_saving_configuration) return true;
    if (!force && !_ctx.file_configs.ui.autosave_enabled)
        return true;
    _configuration_save_error.clear();
    const QScopedValueRollback saving(_saving_configuration,true);
    _update_configuration_save_status();
    const auto report_error=[&](const QString& error) {
        _configuration_save_error = error.isEmpty() ? QStringLiteral("Configuration could not be saved") : error;
        // Publish the completed operation while the reentrancy guard is still held.
        _main_window->set_configuration_save_state(_configuration_load_error.isEmpty()
            ? MainWindow::ConfigurationSaveState::Error : MainWindow::ConfigurationSaveState::LoadError,
            saveAsPath.isEmpty() ? _config_manager.getConfigPath() : QFileInfo(saveAsPath).absoluteFilePath(), _configuration_save_error);
        const auto path=saveAsPath.isEmpty()?_config_manager.getConfigPath():QFileInfo(saveAsPath).absoluteFilePath();
        _main_window->statusBar()->showMessage(ui::text("Could not save settings: %1\n%2").arg(path,error),10000);
        return false;
    };
    const bool recover_source = !_configuration_load_error.isEmpty();
    if (recover_source && saveAsPath.isEmpty())
        return report_error(ui::text("This project could not be opened. Select a current preset and use Save As to create a new project."));
    const auto same_source=[&] {
        const QFileInfo target(saveAsPath),source(_config_manager.getConfigPath());
        const auto identity=[](const QFileInfo& file) {return file.exists()?file.canonicalFilePath():file.absoluteFilePath();};
#ifdef Q_OS_WIN
        if(identity(target).compare(identity(source),Qt::CaseInsensitive)==0)return true;
#else
        if(identity(target)==identity(source))return true;
#endif
        std::error_code error;
        return std::filesystem::equivalent(std::filesystem::path(target.absoluteFilePath().toStdWString()),
            std::filesystem::path(source.absoluteFilePath().toStdWString()),error) && !error;
    };
    if (recover_source && same_source())
        return report_error(ui::text("The unreadable source is preserved. Choose a different file for the new project."));
    auto* model=dynamic_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get());
    if (_numeric_draft_loaded || (model && model->has_numeric_nodes())) {
        auto* editor=_main_window->findChild<SchemeEditorWidget*>();
        QString error;
        domain::NumericProcessingGraph draft;
        if (!editor || !editor->numeric_draft(draft,&error)) return report_error(error);
        editor->flush_presentation();
        std::unique_ptr<desktop::NumericProjectDocument> new_document;
        if (!_numeric_document || recover_source) {
            if(recover_source && draft.nodes.empty())
                return report_error(ui::text("This project could not be opened. Select a current preset and use Save As to create a new project."));
            new_document=std::make_unique<desktop::NumericProjectDocument>();
            const auto result=desktop::create_numeric_project(draft,{},*new_document);
            if (!result) return report_error(result.error);
        }
        auto& document=new_document?*new_document:*_numeric_document;
        desktop::NumericDraftPresentation before,delta;
        delta.applied_graph=_presenter->applied_numeric_graph();
        const auto read=desktop::read_numeric_presentation(document,before);
        if (!read) return report_error(read.error);
        for (auto id:model->allNodeIds()) {
            const auto point=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();
            const QJsonObject position{{"x",point.x()},{"y",point.y()}};
            const auto changed=configuration::changed_fields(before.positions.value(QString::number(id)).toObject(),position);
            if (!changed.isEmpty()) delta.positions[QString::number(id)]=changed;
            if(auto* node=model->delegateModel<AudioNodeModel>(id)) {
                const auto adapter=node->adapter();
                if(before.node_adapters.value(QString::number(id)).toObject()!=adapter)
                    delta.node_adapters[QString::number(id)]=adapter;
                const auto presentation=node->presentation();
                const QJsonObject edited{{"inline_parameters",presentation.value("inline_parameters").toArray()},
                                         {"display_name",presentation.value("display_name").toString()},
                                         {"scalar_inputs",presentation.value("scalar_inputs").toObject()},
                                         {"stream_names",presentation.value("stream_names").toArray()}};
                const auto changed_presentation=configuration::changed_fields(
                    before.node_presentations.value(QString::number(id)).toObject(),edited);
                if(!changed_presentation.isEmpty())delta.node_presentations[QString::number(id)]=changed_presentation;
            }
        }
        delta.editor_view=configuration::changed_fields(before.editor_view,_ctx.file_configs.ui.editor_view);
        if (!_config_manager.numericSettingsDelta(document,_ctx.file_configs,delta.application_settings,&error))
            return report_error(error);
        const bool saved=saveAsPath.isEmpty()?_config_manager.saveNumeric(document,draft,delta,&error)
            :_config_manager.saveNumericAs(document,draft,delta,saveAsPath,&error);
        if (!saved) return report_error(error);
        if (new_document) _numeric_document=std::move(new_document);
        if(recover_source) {
            _configuration_load_error.clear();
            _presenter->clear_file_protection_warning();
        }
        _numeric_draft_loaded=true;
        _ctx.editor_draft_modified=false;
        _configuration_project_dirty=false;
        _configuration_preferences_dirty=false;
        _main_window->set_configuration_save_state(MainWindow::ConfigurationSaveState::Saved, _config_manager.getConfigPath());
        return true;
    }
    return report_error(QStringLiteral("Canonical numeric editor is unavailable"));
}
void CoreApplication::save_configuration_as(const QString &path) {
    if (path.trimmed().isEmpty())
        return;
    if (_save_configuration(true, path))
        _main_window->statusBar()->showMessage(ui::text("Settings saved: %1").arg(_config_manager.getConfigPath()),
                                               4000);
}

void CoreApplication::open_configuration(const QString &path) {
    if (path.trimmed().isEmpty() || _opening_configuration)
        return;
    const auto absolutePath = QFileInfo(path).absoluteFilePath();
    QString error;
    ConfigManager manager(absolutePath);
    desktop::NumericProjectDocument source;
    domain::NumericProcessingGraph draft;
    bool valid=manager.loadNumeric(source,draft,&error);
    if (!valid) {
        _main_window->statusBar()->showMessage(ui::text("Could not open settings: %1\n%2").arg(absolutePath, error),
                                               10000);
        return;
    }
    QStringList arguments{"--config", absolutePath};
    arguments<<_serial_arguments;
    if (_presenter->is_remote()) {
        const auto endpoint = _presenter->remote_endpoint();
        const auto host = endpoint["address"].toString();
        const int port = endpoint["port"].toInt();
        if (host.isEmpty() || port < 1 || port > 65535) {
            _main_window->statusBar()->showMessage(ui::text("The selected remote engine endpoint is unavailable."),
                                                   10000);
            return;
        }
        arguments << "--remote-host" << host << "--remote-port" << QString::number(port);
    }
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments, QDir::currentPath())) {
        _main_window->statusBar()->showMessage(
            ui::text("Could not start the application with settings: %1").arg(absolutePath), 10000);
        return;
    }
    _opening_configuration = true;
    QTimer::singleShot(0, this, [this] {
        _main_window->close();
        QCoreApplication::quit();
    });
}

CoreApplication::~CoreApplication() {
    // The normal close path flushes editor preferences and workspace while
    // every widget is alive. Destructor flush callbacks must not recurse into
    // a partially destroyed window afterward.
    if(_main_window)_main_window->close();
    if(_presenter)disconnect(_presenter.get(),&GuiPresenter::on_config_updated,this,nullptr);
    auto *window=_main_window;
    _main_window = nullptr;
    delete window;
    _presenter.reset();
}
void CoreApplication::_parse_command_line() {
    QCommandLineParser parser;
    parser.setApplicationDescription("Data Processing System GUI");
    const auto help = parser.addHelpOption();
    const auto version = parser.addVersionOption();
    QCommandLineOption config({"c", "config"}, "Configuration file path", "file");
    QCommandLineOption host("remote-host", "Connect to a separate console engine host", "host");
    QCommandLineOption port("remote-port", "Console engine TCP port", "port", "8213");
    parser.addOptions({config, host, port});
    pipeline::add_serial_options(parser);
    // QCommandLineParser::process/showHelp opens a QMessageBox for GUI apps on Windows.
    // Handle terminal-facing arguments explicitly, before config or runtime initialization.
    const auto finish = [](FILE *stream, const QString &message, int status) {
        const auto bytes = message.toUtf8();
        std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stream);
        std::fflush(stream);
        std::exit(status);
    };
    if (!parser.parse(arguments()))
        finish(stderr, parser.errorText() + "\n", 2);
    if (parser.isSet(help))
        finish(stdout, parser.helpText(), 0);
    if (parser.isSet(version))
        finish(stdout, applicationName() + " " + applicationVersion() + "\n", 0);
    _config_manager.setConfigPath(configuration::active_path(parser.value(config)));
    _remote_host = parser.value(host);
    if(!_remote_host.isEmpty() && (parser.isSet("serial-port") || parser.isSet("serial-baud") || parser.isSet("serial-input-node")))
        finish(stderr,"Serial input belongs to the local graph; choose either --remote-host or Serial options.\n",2);
    // Retain only explicit endpoint overrides until configuration is loaded.
    for(const auto &option:{"serial-port","serial-baud","serial-input-node"})
        if(parser.isSet(option)) _serial_arguments<<QStringLiteral("--")+option<<parser.value(option);
    bool ok = false;
    const auto value = parser.value(port).toUInt(&ok);
    if (!ok || !value || value > 65535)
        finish(stderr, "Invalid --remote-port: expected an integer from 1 to 65535.\n", 2);
    _remote_port = static_cast<quint16>(value);
}
int CoreApplication::exec() const {
    if (_ctx.file_configs.ui.move_to_primary_screen_on_start &&
        _ctx.file_configs.ui.workspace.value("geometry").toString().isEmpty())
        _main_window->move(primaryScreen()->availableGeometry().center() - _main_window->rect().center());
    _main_window->show();
    return QApplication::exec();
}
void CoreApplication::apply_processing_settings() {
    _presenter->apply_processing_settings();
}
pipeline::NumericEngine &CoreApplication::pipeline() {
    auto *engine = _presenter->is_remote() ? nullptr : _engine_provider->local_engine();
    if (!engine)
        throw std::logic_error("Local diagnostic engine unavailable in remote GUI");
    return *engine;
}
void CoreApplication::start_wled_audio_reactive_server() {
    _presenter->start_wled_audio_reactive_server();
}
void CoreApplication::stop_wled_audio_reactive_server() {
    _presenter->stop_wled_audio_reactive_server();
}
void CoreApplication::restart_wled_audio_reactive_server() {
    _presenter->restart_wled_audio_reactive_server();
}
