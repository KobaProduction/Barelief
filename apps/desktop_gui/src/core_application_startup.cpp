#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/settings_widget.h>
#include <core_app/ui/settings_change_request.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/example_presentation.h>
#include <desktop/processor_discovery_adapter.h>
#include <control/numeric_desktop_host.h>
#include <pipeline/serial_config.h>
#include <pipeline/numeric_engine.h>
#include <config/config_document.h>
#include <core/performance.h>
#include <barelief/ui_text.h>
#include <QFile>
#include <QFileInfo>
#include <QCommandLineParser>
#include <QTimer>
#include <QScreen>
#include <QDebug>
#include <cstdlib>
#include <utility>

#include "core_application_state.h"
#include <core_app/product_identity.h>
#include "product_information.h"
#include <QIcon>
static int &prepare_gui_arguments(int &argc) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    return argc;
}

CoreApplication::CoreApplication(int &argc,char *argv[],pipeline::AudioSourceFactory source_factory,desktop::DisplayModeApi display_api)
    : QApplication(prepare_gui_arguments(argc),argv) {
    StartupState state;
    _initialize_display(std::move(display_api));_initialize_identity();
    _prepare_startup(state);_create_editor_models();_load_initial_model(state);
    _compose_engine(state,std::move(source_factory));_bind_editor_capabilities();
    _create_main_window();_bind_window_actions(state);_bind_configuration_events(state);
    _start_runtime(state);_start_heartbeat();
}
void CoreApplication::_prepare_startup(StartupState& state) {
    _read_startup_project(state);_handle_startup_failure(state);_recover_startup_workspace(state);
    _configure_serial_runtime(state);_create_startup_example(state);
    ui::Language::instance().set(_ctx.file_configs.ui.language);
}
void CoreApplication::_bind_window_actions(StartupState& state) {
    _bind_display_settings();_bind_display_events();_bind_default_graph_action();
    _bind_analysis_tools();_bind_editor_actions(state);
}
void CoreApplication::_initialize_display(desktop::DisplayModeApi display_api) {
    _display_api=std::move(display_api);
    _injected_display_api=bool(_display_api.query);
    if(!_display_api.query && platformName()!="offscreen" && platformName()!="minimal")
        _display_api=desktop::native_display_mode_api();
    if(_display_api.query && _display_api.enumerate && _display_api.apply)
        _display_session=std::make_unique<desktop::DisplayRefreshSession>(_display_api);
}
void CoreApplication::_initialize_identity() {
    barelief::initialize_resources();
    setApplicationName(barelief::name);
    setApplicationDisplayName(barelief::title);
    setApplicationVersion(barelief::version);
    setOrganizationName(barelief::publisher);
    setOrganizationDomain("github.com/KobaProduction");
    setWindowIcon(barelief::application_icon());
    _parse_command_line();
}
namespace {
desktop::NumericDocumentResult read_startup_source(const QString& path,desktop::NumericProjectDocument& source) {
    QFile file(path);desktop::NumericDocumentResult result;
    if(!file.open(QIODevice::ReadOnly) || file.size()>16777216){result.error="Cannot read bounded project source";return result;}
    const auto bytes=file.readAll();result=desktop::read_numeric_project(bytes,source);
    if(result){source.active_path=path;source.disk_baseline=bytes;}
    return result;
}
}
void CoreApplication::_read_startup_project(StartupState& state) {
    state.config_path=_config_manager.getConfigPath();state.existing_config=QFileInfo::exists(state.config_path);
    if(!state.existing_config)return;
    auto source=std::make_unique<desktop::NumericProjectDocument>();
    const auto result=read_startup_source(state.config_path,*source);
    _admit_startup_project(state,std::move(source),result);
}
void CoreApplication::_admit_startup_project(StartupState& state,std::unique_ptr<desktop::NumericProjectDocument> source,desktop::NumericDocumentResult result) {
    std::uint32_t edition=0;
    if(result)result=desktop::read_project_graph_edition(*source,edition);
    if(result && edition==domain::ProcessingVersions{}.graph){
        state.loaded=_config_manager.loadNumeric(*source,state.numeric_draft,&state.load_error) &&
            _config_manager.readNumericSettings(*source,_ctx.file_configs,&state.load_error);
        _numeric_draft_loaded=state.loaded;
    }else if(result)state.load_error=QStringLiteral("Canonical numeric graph version %1 is required").arg(domain::ProcessingVersions{}.graph);
    else state.load_error=result.path.isEmpty()?result.error:result.path+": "+result.error;
    if(state.loaded)_numeric_document=std::move(source);
}
void CoreApplication::_handle_startup_failure(StartupState& state) {
    if (!state.loaded) {
        if(!state.existing_config && !QFileInfo::exists(state.config_path))
            qCInfo(core_app_main, "Using default configuration for missing file");
        else {
            state.startup_error=QString("Configuration was not loaded: %1: %2").arg(state.config_path,state.load_error);
            qCWarning(core_app_main).noquote()<<state.startup_error;
            // Preserve storage only. A failed boundary must not populate typed
            // settings or authorize a default processing graph.
            const auto failed=configuration::read(state.config_path);
            _ctx.file_configs.raw_document=failed.document;
        }
    }
}
void CoreApplication::_recover_startup_workspace(StartupState& state) {
    if(!state.startup_error.isEmpty()){
        QString workspace_error;_config_manager.loadWorkspaceRecovery(_ctx.file_configs,&workspace_error);
        // No graph was admitted: serialized ADS groups can refer to docks that
        // cannot be constructed. Restore preferences/closed states in the safe
        // default topology instead of creating orphan floating containers.
        _ctx.file_configs.ui.workspace.remove("ads_state");
        _ctx.file_configs.ui.workspace.remove("state");
        auto& ui=_ctx.file_configs.ui;
        ui.show_raw_samples=ui.show_window_signal=ui.show_windowed_signal=ui.show_window_function=false;
        ui.show_amplitudes=ui.show_test_amplitudes=ui.show_spectrogram=false;
        if(!workspace_error.isEmpty())qCWarning(core_app_main).noquote()<<workspace_error;
    }
}
void CoreApplication::_configure_serial_runtime(StartupState& state) {
    state.runtime_config=_ctx.file_configs.raw_document;
    QCommandLineParser runtime_options;pipeline::add_serial_options(runtime_options);
    QStringList runtime_arguments{applicationName()};runtime_arguments<<_serial_arguments;
    runtime_options.parse(runtime_arguments);
    QString serial_error;
    if(!pipeline::apply_serial_options(runtime_options,state.runtime_config,serial_error)) {
        qCritical().noquote()<<serial_error;std::exit(2);
    }
}
void CoreApplication::_create_startup_example(StartupState& state) {
    state.first_run_demo=!state.existing_config && !QFileInfo::exists(state.config_path) &&
        state.startup_error.isEmpty() && _remote_host.isEmpty() && !pipeline::read_serial_binding(state.runtime_config).configured;
    if(state.first_run_demo) {
        state.first_example=numeric_startup_example();
        state.numeric_draft=state.first_example->graph;
        auto source=std::make_unique<desktop::NumericProjectDocument>();
        const auto created=desktop::create_numeric_startup_project(state.numeric_draft,*source);
        if(!created)state.startup_error=created.error;
        else if(!_config_manager.readNumericSettings(*source,_ctx.file_configs,&state.load_error))state.startup_error=state.load_error;
        else {
            source->active_path=state.config_path;
            _numeric_document=std::move(source);
            _numeric_draft_loaded=true;
            state.runtime_config=_ctx.file_configs.raw_document;
        }
    }
}
void CoreApplication::_create_editor_models() {
    _ctx.delegate_model_registry = std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    _ctx.data_flow_graph = std::make_shared<NumericEditorGraphModel>(_ctx.delegate_model_registry);
    const auto kinds=numeric_editor::catalog();
    for (const auto &kind : kinds) {
        _ctx.delegate_model_registry->registerModel(
            [kind] { return std::make_unique<AudioNodeModel>(kind); }, numeric_editor::category(kind));
    }
}
void CoreApplication::_load_initial_model(StartupState& state) {
    if (!_numeric_draft_loaded) {
        state.numeric_draft.graph_id="main";
        _numeric_draft_loaded=true;
    }
    {
        auto* model=static_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get());
        desktop::NumericDraftPresentation initial_presentation;
        if(_numeric_document) {
            const auto result=desktop::read_numeric_presentation(*_numeric_document,initial_presentation);
            if(!result)state.startup_error=result.error;
        }
        if (!model->set_numeric_graph(state.numeric_draft,&state.load_error,initial_presentation.node_adapters))
            state.startup_error=state.load_error;
        _ctx.file_configs.scene_graph=model->save();
    }
}
void CoreApplication::_compose_engine(StartupState& state,pipeline::AudioSourceFactory source_factory) {
    _engine_provider = std::make_shared<DesktopEngineProvider>(std::move(source_factory));
    const auto serial=pipeline::read_serial_binding(state.runtime_config);
    if(serial.configured) {
        _engine_provider->set_serial_endpoint(serial.endpoint);
        if(_remote_host.isEmpty())_remote_host=serial.endpoint.port_name;
    }
    _presenter = std::make_unique<GuiPresenter>(_ctx, _engine_provider, _remote_host, _remote_port);
    if(state.first_example)_ctx.file_configs.ui.graph_views=example_view_preferences(*state.first_example,
        _presenter->device_id(),QString::fromStdString(state.numeric_draft.graph_id));
    connect(_presenter.get(), &GuiPresenter::on_config_updated, this, &CoreApplication::on_config_updated);
    connect(_presenter.get(), &GuiPresenter::pipeline_preview, this, &CoreApplication::pipeline_preview);
    connect(_presenter.get(), &GuiPresenter::component_status_changed, this,
            &CoreApplication::component_status_changed);
}
void CoreApplication::_bind_editor_capabilities() {
    auto* model=dynamic_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get());if(!model)return;
    const auto* host=dynamic_cast<pipeline::NumericDesktopHost*>(&_presenter->client());
    const auto* engine=host?host->local_engine():nullptr;
    const auto capabilities=engine?engine->capabilities():application::ProcessingCapabilities{};
    model->set_admission_limits(capabilities.numeric
        ?std::optional{capabilities.numeric->runtime.admission}:std::nullopt);
    model->set_capture_device_provider([this](int kind,QString* error){return _capture_devices(kind,error);});
    model->set_node_validator([this](const domain::ConfiguredProcessingNode& node){return _validate_node(node);});
}
QJsonArray CoreApplication::_capture_devices(int kind,QString* error) const {
    QJsonArray choices;
    const auto* host=dynamic_cast<pipeline::NumericDesktopHost*>(&_presenter->client());
    if(!host || !host->local_engine()){if(error)*error=QStringLiteral("Capture device discovery requires the local desktop device");return choices;}
    if(kind<0 || kind>1){if(error)*error=QStringLiteral("Invalid capture source kind");return choices;}
    for(const auto& device:pipeline::enumerate_audio_capture_devices(pipeline::AudioSourceKind(kind),error))
        choices.append(QJsonObject{{"id",device.id},{"name",device.name},{"is_default",device.is_default},
            {"native_sample_rate",int(device.native_sample_rate)},{"native_channels",int(device.native_channels)}});
    return choices;
}
domain::ProcessingDiagnostic CoreApplication::_validate_node(const domain::ConfiguredProcessingNode& node) const {
    auto* host=dynamic_cast<pipeline::NumericDesktopHost*>(&_presenter->client());
    return host?host->validate_numeric_node(node):domain::ProcessingDiagnostic{};
}
void CoreApplication::_create_main_window() {
    _processor_discovery = std::make_shared<desktop::ProcessorDiscoveryAdapter>();
    _engine_provider->set_registry(_processor_discovery->client());
    _main_window = new MainWindow(*_presenter, _processor_discovery);
    barelief::install_product_information(*_main_window);
}

void CoreApplication::_restore_startup_draft(StartupState& state,SchemeEditorWidget* editor) {
    if(!_numeric_draft_loaded || !state.startup_error.isEmpty())return;
    desktop::NumericDraftPresentation presentation;
    const auto result=_numeric_document?desktop::read_numeric_presentation(*_numeric_document,presentation):desktop::NumericDocumentResult{};
    if(!result){state.startup_error=result.error;return;}
    if(!editor->load_numeric_draft(state.numeric_draft,&state.load_error,presentation.node_adapters)){state.startup_error=state.load_error;return;}
    _restore_startup_presentation(state,editor,presentation);
}
void CoreApplication::_bind_configuration_events(StartupState& state) {
    _configuration_load_error=state.startup_error;
    _configuration_project_dirty = !state.existing_config;
    _configuration_save_error = state.startup_error;
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
    _bind_server_events();
}
void CoreApplication::_bind_server_events() {
    connect(_main_window, &MainWindow::start_wled_server_requested, _presenter.get(),
            &GuiPresenter::start_wled_audio_reactive_server);
    connect(_main_window, &MainWindow::stop_wled_server_requested, _presenter.get(),
            &GuiPresenter::stop_wled_audio_reactive_server);
    connect(_main_window, &MainWindow::wled_server_config_changed, _presenter.get(),
            &GuiPresenter::restart_wled_audio_reactive_server);
}
void CoreApplication::_start_runtime(StartupState& state) {
    _presenter->initialize(std::move(state.runtime_config),state.startup_error);
    if(state.startup_error.isEmpty())
        QTimer::singleShot(0,this,[this] {
            // Every valid project starts through the same transactional runtime path.
            _presenter->apply_pipeline_graph({});
        });
}
void CoreApplication::_start_heartbeat() {
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

void CoreApplication::_restore_startup_presentation(StartupState& state,SchemeEditorWidget* editor,const desktop::NumericDraftPresentation& presentation) {
                if(state.first_example)apply_example_labels(dynamic_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get()),*state.first_example);
                editor->restore_node_positions(presentation.positions);
                for(auto it=presentation.node_presentations.begin();it!=presentation.node_presentations.end();++it)
                    if(auto* node=_ctx.data_flow_graph->delegateModel<AudioNodeModel>(it.key().toUInt()))
                        node->set_presentation(it.value().toObject());
                if(state.first_run_demo || presentation.positions.isEmpty())editor->initialize_default_layout();
                _ctx.editor_draft_modified=false;
}
