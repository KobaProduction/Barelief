#include "core_app/core_application.h"
#include "core_app/main_window.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
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
#include "pipeline/serial_config.h"

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
        if (frame.parent)
            frame.parent->children += duration;
        gui_event_profile::record(name.data(), type, duration, frame.children);
    }
    core::perf::record(core::perf::Stage::GuiEvent, start, duration);
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
    if (!_config_manager.load(_ctx.file_configs))
        qCInfo(core_app_main, "Using default configuration");
    auto runtime_config=_ctx.file_configs.raw_document;
    QCommandLineParser runtime_options;pipeline::add_serial_options(runtime_options);
    QStringList runtime_arguments{applicationName()};runtime_arguments<<_serial_arguments;
    runtime_options.parse(runtime_arguments);
    QString serial_error;
    if(!pipeline::apply_serial_options(runtime_options,runtime_config,serial_error)) {
        qCritical().noquote()<<serial_error;std::exit(2);
    }
    _ctx.delegate_model_registry = std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    _ctx.data_flow_graph = std::make_shared<QtNodes::DataFlowGraphModel>(_ctx.delegate_model_registry);
    for (const auto &definition : pipeline::node_definitions()) {
        _ctx.delegate_model_registry->registerModel(
            [kind = definition.name] { return std::make_unique<AudioNodeModel>(kind); }, definition.category);
    }
    if (_ctx.file_configs.scene_graph.isEmpty() && !_ctx.file_configs.raw_document.contains("scene_graph")) {
        QString graph_error;
        _ctx.file_configs.scene_graph = pipeline::GraphProgram::compile(_ctx.file_configs.pipeline_graph, graph_error)
                                            ? _ctx.file_configs.pipeline_graph
                                            : pipeline::GraphProgram::defaults(_ctx.file_configs.core.spectrum_channel,
                                                                               _ctx.file_configs.core.spectrum_merge,
                                                                               _ctx.file_configs.core.capture_source);
    }

    if (not _ctx.file_configs.scene_graph.empty()) {
        const auto loaded_document = pipeline::canonical_graph_document(_ctx.file_configs.scene_graph);
        for (const auto &value : loaded_document.value("nodes").toArray()) {
            const auto node = value.toObject();
            const auto id = node.value("id").toInt(-1);
            const auto kind = node.value("internal-data").toObject().value("model-name").toString();
            if (id < 0 || id == std::numeric_limits<int>::max() || _ctx.data_flow_graph->nodeExists(id))
                continue;
            if (pipeline::node_definition(kind).name.isEmpty())
                continue; // An unknown saved model remains in the preserved draft document.
            try {
                _ctx.data_flow_graph->loadNode(node);
            } catch (const std::exception &error) {
                qCWarning(core_app_main, "Ignoring invalid saved node: %s", error.what());
            }
        }
        for (const auto &value : loaded_document.value("connections").toArray()) {
            const auto connection = QtNodes::fromJson(value.toObject());
            if (_ctx.data_flow_graph->nodeExists(connection.inNodeId) &&
                _ctx.data_flow_graph->nodeExists(connection.outNodeId) &&
                _ctx.data_flow_graph->connectionPossible(connection))
                _ctx.data_flow_graph->addConnection(connection);
        }
    }

    _engine_provider = std::make_shared<DesktopEngineProvider>(std::move(source_factory));
    _presenter = std::make_unique<GuiPresenter>(_ctx, _engine_provider, _remote_host, _remote_port);
    connect(_presenter.get(), &GuiPresenter::on_config_updated, this, &CoreApplication::on_config_updated);
    connect(_presenter.get(), &GuiPresenter::pipeline_preview, this, &CoreApplication::pipeline_preview);
    connect(_presenter.get(), &GuiPresenter::component_status_changed, this,
            &CoreApplication::component_status_changed);
    connect(_presenter.get(), &GuiPresenter::raw_signal_ready, this, &CoreApplication::raw_signal_ready);
    connect(_presenter.get(), &GuiPresenter::window_signal_ready, this, &CoreApplication::window_signal_ready);
    connect(_presenter.get(), &GuiPresenter::windowed_signal_ready, this, &CoreApplication::windowed_signal_ready);
    connect(_presenter.get(), &GuiPresenter::window_function_ready, this, &CoreApplication::window_function_ready);
    connect(_presenter.get(), &GuiPresenter::amplitudes_ready, this, &CoreApplication::amplitudes_ready);
    connect(_presenter.get(), &GuiPresenter::test_amplitudes_ready, this, &CoreApplication::test_amplitudes_ready);
    connect(_presenter.get(), &GuiPresenter::spectrogram_updated, this, &CoreApplication::spectrogram_updated);
    connect(_presenter.get(), &GuiPresenter::sample_rate_changed, this, &CoreApplication::sample_rate_changed);
    connect(_presenter.get(), &GuiPresenter::analyzer_started, this, &CoreApplication::analyzer_started);
    connect(_presenter.get(), &GuiPresenter::analyzer_stopped, this, &CoreApplication::analyzer_stopped);
    connect(_presenter.get(), &GuiPresenter::loopback_started, this, &CoreApplication::loopback_started);
    connect(_presenter.get(), &GuiPresenter::loopback_stopped, this, &CoreApplication::loopback_stopped);
    _main_window = new MainWindow(*_presenter);
    connect(_main_window, &MainWindow::on_config_updated, this, [this] { _save_configuration(true); });
    connect(_presenter.get(), &GuiPresenter::on_config_updated, this, [this] { _save_configuration(false); });
    connect(_main_window, &MainWindow::save_configuration_as_requested, this, &CoreApplication::save_configuration_as);
    connect(_main_window, &MainWindow::open_configuration_requested, this, &CoreApplication::open_configuration);
    connect(_main_window, &MainWindow::start_wled_server_requested, _presenter.get(),
            &GuiPresenter::start_wled_audio_reactive_server);
    connect(_main_window, &MainWindow::stop_wled_server_requested, _presenter.get(),
            &GuiPresenter::stop_wled_audio_reactive_server);
    connect(_main_window, &MainWindow::wled_server_config_changed, _presenter.get(),
            &GuiPresenter::restart_wled_audio_reactive_server);
    _presenter->initialize(std::move(runtime_config));
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
bool CoreApplication::_save_configuration(bool force, const QString &saveAsPath) {
    if (!force && !_ctx.file_configs.ui.autosave_enabled)
        return true;
    auto current_config = _ctx.file_configs;
    if (_ctx.editor_draft_modified) {
        if (auto *editor = _main_window->findChild<SchemeEditorWidget *>())
            current_config.scene_graph = editor->document();
        else
            current_config.scene_graph = _ctx.data_flow_graph->save();
        // Override an initial-layout marker in the preserved original JSON.
        // Omitting it would let recursive settings overlay resurrect `true`.
        current_config.scene_graph["layout_pending"] = false;
    }
    if (!_presenter->device_id().isEmpty()) {
        auto devices = current_config.raw_document.value("devices").toObject();
        auto device = devices.value(_presenter->device_id()).toObject();
        device["scene_graph"] = current_config.scene_graph;
        device["pipeline_graph"] = current_config.pipeline_graph;
        device["presets"] = current_config.ui.pipeline_presets;
        device["editor_view"] = current_config.ui.editor_view;
        devices[_presenter->device_id()] = device;
        current_config.raw_document["devices"] = devices;
    }
    auto saved_config = current_config;
    if (_presenter->is_remote()) {
        const auto local =
            saved_config.raw_document.value("devices").toObject().value(_presenter->local_device_id()).toObject();
        saved_config.scene_graph = local.contains("scene_graph") ? local["scene_graph"].toObject()
                                                                 : saved_config.raw_document["scene_graph"].toObject();
        saved_config.pipeline_graph = local.contains("pipeline_graph")
                                          ? local["pipeline_graph"].toObject()
                                          : saved_config.raw_document["pipeline_graph"].toObject();
        if (local.contains("presets"))
            saved_config.ui.pipeline_presets = local["presets"].toArray();
        if (local.contains("editor_view"))
            saved_config.ui.editor_view = local["editor_view"].toObject();
    }
    QString error;
    const bool saved = saveAsPath.isEmpty() ? _config_manager.save(saved_config, &error)
                                            : _config_manager.saveAs(saved_config, saveAsPath, &error);
    if (!saved) {
        const auto path =
            saveAsPath.isEmpty() ? _config_manager.getConfigPath() : QFileInfo(saveAsPath).absoluteFilePath();
        _main_window->statusBar()->showMessage(ui::text("Could not save settings: %1\n%2").arg(path, error), 10000);
        return false;
    }
    _ctx.file_configs = current_config;
    _ctx.editor_draft_modified = false;
    return true;
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
    FileSaveApplicationConfig candidate;
    QString error;
    if (!ConfigManager(absolutePath).load(candidate, &error)) {
        _main_window->statusBar()->showMessage(ui::text("Could not open settings: %1\n%2").arg(absolutePath, error),
                                               10000);
        return;
    }
    QStringList arguments{"--config", absolutePath};
    if(!_presenter->is_remote()) arguments<<_serial_arguments;
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
    delete _main_window;
    _main_window = nullptr;
    _presenter.reset();
}
void CoreApplication::_parse_command_line() {
    QCommandLineParser parser;
    parser.setApplicationDescription("Data Processing System GUI");
    const auto help = parser.addHelpOption();
    const auto version = parser.addVersionOption();
    QCommandLineOption config({"c", "config"}, "Configuration file path", "file", "config.json");
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
    _config_manager.setConfigPath(parser.value(config));
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
pipeline::AudioPipeline &CoreApplication::pipeline() {
    auto *engine = _presenter->is_remote() ? nullptr : _engine_provider->local_engine();
    if (!engine)
        throw std::logic_error("Local diagnostic engine unavailable in remote GUI");
    return *engine;
}
void CoreApplication::start_analyzer() {
    _presenter->start_analyzer();
}
void CoreApplication::stop_analyzer() {
    _presenter->stop_analyzer();
}
void CoreApplication::start_loopback() {
    _presenter->start_loopback();
}
void CoreApplication::stop_loopback() {
    _presenter->stop_loopback();
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
