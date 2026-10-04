#include "core_app/core_application.h"
#include "core_app/main_window.h"
#include "core_app/scheme_editor/processor_data_model.h"
#include "core_app/scheme_editor/sink_data_model.h"
#include "core_app/scheme_editor/source_data_model.h"
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

Q_LOGGING_CATEGORY(core_app_main, "core_app.main")
CoreApplication::CoreApplication(int &argc, char *argv[]) : QApplication(argc, argv) {
    setApplicationName("Data Processing System Core");
    setApplicationVersion("1.0.0");
    setOrganizationName("Data Processing System");
    _parse_command_line();
    if (!_config_manager.load(_ctx.file_configs))
        qCInfo(core_app_main, "Using default configuration");
    _ctx.delegate_model_registry = std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    _ctx.data_flow_graph = std::make_shared<QtNodes::DataFlowGraphModel>(_ctx.delegate_model_registry);
    _ctx.delegate_model_registry->registerModel<NumberSourceDataModel>();
    _ctx.delegate_model_registry->registerModel<NumberDisplayDataModel>();
    _ctx.delegate_model_registry->registerModel<AdditionModel>();
    for (const auto &definition : pipeline::node_definitions()) {
        _ctx.delegate_model_registry->registerModel(
            [kind = definition.name] { return std::make_unique<AudioNodeModel>(kind); }, "Audio pipeline");
    }
    if (_ctx.file_configs.scene_graph.isEmpty() && !_ctx.file_configs.raw_document.contains("scene_graph"))
        _ctx.file_configs.scene_graph = pipeline::GraphProgram::defaults(_ctx.file_configs.core.spectrum_channel,
                                                                         _ctx.file_configs.core.spectrum_merge,
                                                                         _ctx.file_configs.core.capture_source);

    if (not _ctx.file_configs.scene_graph.empty()) {
        for (const auto &value : _ctx.file_configs.scene_graph.value("nodes").toArray()) {
            const auto node = value.toObject();
            const auto id = node.value("id").toInt(-1);
            const auto kind = node.value("internal-data").toObject().value("model-name").toString();
            if (id < 0 || id == std::numeric_limits<int>::max() || _ctx.data_flow_graph->nodeExists(id))
                continue;
            if (pipeline::node_definition(kind).name.isEmpty() && kind != "NumberSource" &&
                kind != "Result" && kind != "Addition")
                continue; // An unknown saved model remains in the preserved draft document.
            try {
                _ctx.data_flow_graph->loadNode(node);
            } catch (const std::exception &error) {
                qCWarning(core_app_main, "Ignoring invalid saved node: %s", error.what());
            }
        }
        for (const auto &value : _ctx.file_configs.scene_graph.value("connections").toArray()) {
            const auto connection = QtNodes::fromJson(value.toObject());
            if (_ctx.data_flow_graph->nodeExists(connection.inNodeId) &&
                _ctx.data_flow_graph->nodeExists(connection.outNodeId) &&
                _ctx.data_flow_graph->connectionPossible(connection))
                _ctx.data_flow_graph->addConnection(connection);
        }
    }

    _presenter = std::make_unique<GuiPresenter>(_ctx, _remote_host, _remote_port);
    connect(_presenter.get(), &GuiPresenter::on_config_updated, this, &CoreApplication::on_config_updated);
    connect(_presenter.get(), &GuiPresenter::pipeline_preview, this, &CoreApplication::pipeline_preview);
    connect(_presenter.get(), &GuiPresenter::component_status_changed, this,
            &CoreApplication::component_status_changed);
    connect(_presenter.get(), &GuiPresenter::raw_samples_ready, this, &CoreApplication::raw_samples_ready);
    connect(_presenter.get(), &GuiPresenter::samples_ready, this, &CoreApplication::samples_ready);
    connect(_presenter.get(), &GuiPresenter::samples_after_window_function_ready, this,
            &CoreApplication::samples_after_window_function_ready);
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
    auto save = [this](bool force) {
        if (_ctx.editor_draft_modified) {
            _ctx.file_configs.scene_graph = _ctx.data_flow_graph->save();
            _ctx.file_configs.scene_graph["version"] = 1;
            _ctx.editor_draft_modified = false;
        }
        if ((force || _ctx.file_configs.ui.autosave_enabled) && !_config_manager.save(_ctx.file_configs))
            _main_window->statusBar()->showMessage("Could not save settings: " + _config_manager.getConfigPath(),
                                                   10000);
    };
    connect(_main_window, &MainWindow::on_config_updated, this, [save] { save(true); });
    connect(_presenter.get(), &GuiPresenter::on_config_updated, this, [save] { save(false); });
    connect(_main_window, &MainWindow::start_loopback_requested, _presenter.get(), &GuiPresenter::start_loopback);
    connect(_main_window, &MainWindow::stop_loopback_requested, _presenter.get(), &GuiPresenter::stop_loopback);
    connect(_main_window, &MainWindow::start_analyzer_requested, _presenter.get(), &GuiPresenter::start_analyzer);
    connect(_main_window, &MainWindow::stop_analyzer_requested, _presenter.get(), &GuiPresenter::stop_analyzer);
    connect(_main_window, &MainWindow::start_wled_server_requested, _presenter.get(),
            &GuiPresenter::start_wled_audio_reactive_server);
    connect(_main_window, &MainWindow::stop_wled_server_requested, _presenter.get(),
            &GuiPresenter::stop_wled_audio_reactive_server);
    connect(_main_window, &MainWindow::wled_server_config_changed, _presenter.get(),
            &GuiPresenter::restart_wled_audio_reactive_server);
    _presenter->initialize();
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
    bool ok = false;
    const auto value = parser.value(port).toUInt(&ok);
    if (!ok || !value || value > 65535)
        finish(stderr, "Invalid --remote-port: expected an integer from 1 to 65535.\n", 2);
    _remote_port = static_cast<quint16>(value);
}
int CoreApplication::exec() const {
    if (_ctx.file_configs.ui.move_to_primary_screen_on_start)
        _main_window->move(primaryScreen()->availableGeometry().center() - _main_window->rect().center());
    _main_window->showNormal();
    return QApplication::exec();
}
void CoreApplication::apply_processing_settings() {
    _presenter->apply_processing_settings();
}
pipeline::AudioPipeline &CoreApplication::pipeline() {
    auto *engine = _presenter->client().local_engine();
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
