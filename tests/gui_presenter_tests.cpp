#include "synthetic_engine_provider.h"
#include "core_app/config_manager.h"
#include "core_app/gui_presenter.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core/network/tcp_packet_socket.h"
#include <QTcpServer>
#include <QDataStream>
#include <QCoreApplication>
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <stdexcept>
#include <algorithm>
#include "pipeline/engine_port.h"
#include <QTimer>

class FakeEndpoint final : public pipeline::EnginePort {
public:
    pipeline::EngineSnapshot current;
    std::vector<pipeline::EngineCommand> commands;
    std::function<void(pipeline::EngineSnapshot)> snapshots;
    std::function<void(pipeline::EngineReply)> replies;
    std::function<void(pipeline::ViewFrame)> views;
    std::function<void(pipeline::AudioFrame)> audio;
    QJsonObject configured;
    int connections = 0;
    quint64 next = 0;
    quint64 submit(pipeline::EngineCommand command) override {
        commands.push_back(command);
        const auto id = ++next;
        if (command.kind == pipeline::EngineCommand::ApplyGraph) {
            QTimer::singleShot(0, [this, id, command] {
                current.graph = command.graph;
                ++current.graph_revision;
                ++current.revision;
                if (snapshots) snapshots(current);
                if (replies) replies({id, true, {}, current});
            });
        }
        return id;
    }
    void set_snapshot_handler(std::function<void(pipeline::EngineSnapshot)> h) override { snapshots = std::move(h); }
    void set_reply_handler(std::function<void(pipeline::EngineReply)> h) override { replies = std::move(h); }
    void set_view_handler(std::function<void(pipeline::ViewFrame)> h) override { views = std::move(h); }
    void set_audio_handler(std::function<void(pipeline::AudioFrame)> h) override { audio = std::move(h); }
    pipeline::EngineSnapshot snapshot() const override { return current; }
    bool configure_network(QJsonObject config, QString *) override { configured = std::move(config); return true; }
    QString network_warning() const override { return {}; }
    void connect_remote() override { ++connections; if (snapshots) snapshots(current); }
    void disconnect_remote() override {}
};

class FakeProvider final : public pipeline::EngineProvider {
public:
    struct Selection { QString host; quint16 port; FakeEndpoint *endpoint; };
    std::vector<Selection> selections;
    std::unique_ptr<pipeline::EnginePort> create(const QString &host, quint16 port) override {
        auto endpoint = std::make_unique<FakeEndpoint>();
        endpoint->current.epoch = selections.size() + 1;
        endpoint->current.graph = pipeline::GraphProgram::defaults();
        selections.push_back({host, port, endpoint.get()});
        return endpoint;
    }
};

static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class Predicate> static void wait(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), "Presenter reply timed out");
}
static void load_editor(GuiState &state, const QJsonObject &graph) {
    state.delegate_model_registry = std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for (const auto &definition : pipeline::node_definitions())
        state.delegate_model_registry->registerModel(
            [kind = definition.name] { return std::make_unique<AudioNodeModel>(kind); }, "Audio pipeline");
    state.data_flow_graph = std::make_shared<QtNodes::DataFlowGraphModel>(state.delegate_model_registry);
    state.data_flow_graph->load(graph);
}
// A real BDSP peer with explicitly delayed replies exercises Presenter ordering,
// rather than replacing EngineClient's production callbacks with a test hook.
class DelayedEnginePeer final : public QTcpServer {
public:
    struct Request { quint64 id, revision; QJsonObject graph; };
    std::vector<Request> requests;
    std::vector<quint8> commands;
    QJsonObject active = pipeline::GraphProgram::defaults();
    TcpBDSPSocket *peer = nullptr;
    void reply(const Request &request, bool accepted = true) {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
        stream << quint16(1) << request.id << accepted << (accepted ? QString{} : QString("Rejected"));
        stream << quint64(123) << request.revision << request.revision << quint64(0) << quint64(0)
               << QJsonDocument(request.graph).toJson(QJsonDocument::Compact) << quint32(0) << false;
        require(peer->send_packet(6, bytes), "Cannot send delayed reply");
    }
protected:
    void incomingConnection(qintptr descriptor) override {
        peer = new TcpBDSPSocket(this);
        require(peer->setSocketDescriptor(descriptor), "Cannot accept delayed peer");
        QObject::connect(peer, &TcpBDSPSocket::packetReceived, this, [this](quint8 packet, QByteArray bytes) {
            require(packet == 5, "Unexpected delayed peer packet");
            QDataStream stream(bytes);
            stream.setVersion(QDataStream::Qt_6_0);
            stream.setByteOrder(QDataStream::LittleEndian);
            stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
            quint16 version, port; quint64 id, expected; quint8 kind;
            qint32 value; quint32 subscriptions; QString interface_address, destination; QByteArray graph;
            stream >> version >> id >> kind >> expected >> value >> subscriptions >> interface_address >> destination >> port >> graph;
            require(stream.status() == QDataStream::Ok && version == 1, "Invalid delayed peer command");
            commands.push_back(kind);
            if (kind == pipeline::EngineCommand::ApplyGraph)
                requests.push_back({id, quint64(requests.size() + 1), QJsonDocument::fromJson(graph).object()});
            else
                reply({id, 0, active});
        });
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (QCoreApplication::arguments().contains("--replaceable-port-only")) {
        GuiState state;
        state.file_configs.pipeline_graph = pipeline::GraphProgram::defaults();
        state.file_configs.scene_graph = state.file_configs.pipeline_graph;
        state.file_configs.raw_document = {{"unknown", 42}};
        auto provider = std::make_shared<FakeProvider>();
        GuiPresenter presenter(state, provider);
        auto *local = provider->selections.at(0).endpoint;
        require(provider->selections.at(0).host.isEmpty(), "Local bootstrap bypassed provider");
        const auto runtime = QJsonObject{{"runtime-only", true}};
        presenter.initialize(runtime);
        wait([&] { return !presenter.graph_request_pending(); });
        require(local->configured == runtime, "Injected endpoint did not receive runtime config");
        require(local->commands.size() >= 2 && local->commands.back().kind == pipeline::EngineCommand::ApplyGraph,
                "Bootstrap commands did not reach endpoint");
        const auto local_draft = QJsonObject{{"version", 2}, {"custom-draft", 7}};
        state.file_configs.scene_graph = local_draft;
        state.file_configs.ui.editor_view = {{"zoom", .5}};
        presenter.select_device("remote", "fake-host", 9123);
        require(provider->selections.size() == 2 && provider->selections.back().host == "fake-host" &&
                provider->selections.back().port == 9123, "Dynamic selection bypassed provider");
        auto *remote = provider->selections.back().endpoint;
        require(remote->connections == 1 && state.file_configs.scene_graph == remote->current.graph,
                "Remote snapshot was not handled");
        int delivered_views = 0, running_notifications = 0;
        QObject::connect(&presenter, &GuiPresenter::node_views_ready, &presenter,
                         [&](const auto &) { ++delivered_views; });
        QObject::connect(&presenter, &GuiPresenter::analyzer_started, &presenter,
                         [&] { ++running_notifications; });
        remote->current.statuses["Analyzer"] = {pipeline::State::Running, {}, 1};
        remote->current.window = {1, 2};
        remote->current.frequency_step = 12;
        remote->snapshots(remote->current);
        require(running_notifications == 1 && presenter.window() == remote->current.window &&
                presenter.frequency_step() == 12, "Snapshot status/data did not cross injected port");
        pipeline::ViewFrame view;
        view.epoch = remote->current.epoch;
        view.graph_revision = remote->current.graph_revision;
        remote->views(view);
        require(delivered_views == 1, "View handler did not cross injected port");
        require(local->commands.back().kind == pipeline::EngineCommand::Subscribe &&
                local->commands.back().subscriptions == 0, "Inactive endpoint retained view subscription");
        presenter.stop_analyzer();
        require(remote->commands.back().kind == pipeline::EngineCommand::StopAnalyzer,
                "Selected endpoint did not receive command");
        const auto remote_draft = QJsonObject{{"version", 2}, {"remote-draft", 9}};
        state.file_configs.scene_graph = remote_draft;
        presenter.select_device(presenter.local_device_id(), {}, 8213);
        require(provider->selections.size() == 2 && &presenter.client() == local,
                "Returning device recreated the endpoint");
        require(state.file_configs.scene_graph == local_draft && state.file_configs.ui.editor_view["zoom"].toDouble() == .5,
                "Local draft/view restoration changed");
        auto stale = remote->current; stale.graph["stale"] = true;
        remote->snapshots(stale);
        require(state.file_configs.scene_graph == local_draft, "Inactive snapshot changed selected draft");
        remote->views(view);
        require(delivered_views == 1, "Inactive endpoint delivered a view");
        presenter.select_device("remote", "fake-host", 9123);
        require(provider->selections.size() == 2 && &presenter.client() == remote &&
                state.file_configs.scene_graph == remote_draft, "Remote draft/endpoint restoration changed");
        require(state.file_configs.raw_document["unknown"].toInt() == 42, "Selection lost unknown config fields");
        auto direct_provider = std::make_shared<FakeProvider>();
        GuiState direct;
        direct.file_configs.pipeline_graph = pipeline::GraphProgram::defaults();
        GuiPresenter direct_remote(direct, direct_provider, "initial-host", 9234);
        direct_remote.initialize();
        require(direct_provider->selections.size() == 1 && direct_provider->selections[0].host == "initial-host" &&
                direct_provider->selections[0].endpoint->connections == 1,
                "Initial remote endpoint bypassed provider");
        return 0;
    }
    if(QCoreApplication::arguments().contains("--serial-runtime-options-only")) {
        GuiState state;
        state.file_configs.pipeline_graph=QJsonObject{{"version",2},{"nodes",QJsonArray{
            QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","mel"},{"stream_id","uart"}}}},
            QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","MelView"}}}}}},
            {"connections",QJsonArray{QJsonObject{{"outNodeId",0},{"inNodeId",1},{"outPortIndex",0},{"inPortIndex",0}}}}};
        state.file_configs.raw_document={{"unknown",42},{"network",QJsonObject{{"unknown-network",true}}}};
        const auto saved=state.file_configs.raw_document;
        auto serial_provider = test_fixture::synthetic_engine_provider();
        GuiPresenter presenter(state, serial_provider);
        QString rejected;
        QObject::connect(&presenter,&GuiPresenter::command_rejected,[&](quint64,const QString &error){rejected=error;});
        auto runtime_config=saved;
        runtime_config["network"]=QJsonObject{{"serial",QJsonObject{{"port_name",""},{"input_node",0}}}};
        presenter.initialize(runtime_config);
        wait([&]{return !presenter.graph_request_pending();});
        require(rejected.contains("port_name"),"Presenter ignored runtime-only Serial config");
        require(state.file_configs.raw_document==saved,"Runtime Serial overrides changed persisted config");
        require(serial_provider->local_engine()->current_status("Capture").state==pipeline::State::Stopped,
                "Serial-only initialization opened a capture source");
        return 0;
    }
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create CLI test directory");
#ifdef Q_OS_WIN
    const auto gui_executable = QCoreApplication::applicationDirPath() + "/core_app.exe";
#else
    const auto gui_executable = QCoreApplication::applicationDirPath() + "/core_app";
#endif
    auto check_cli = [&](const QStringList &arguments, int exit_code, const QByteArray &expected, bool error) {
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(directory.path());
        process.start(gui_executable, arguments);
        require(process.waitForStarted(3000), "GUI CLI subprocess did not start");
        const auto finished = process.waitForFinished(3000);
        if (!finished) {
            process.kill();
            process.waitForFinished(3000);
        }
        require(finished, "GUI CLI subprocess blocked, possibly waiting for a modal dialog");
        require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == exit_code,
                "GUI CLI subprocess returned an unexpected exit status");
        const auto output = error ? process.readAllStandardError() : process.readAllStandardOutput();
        require(output.contains(expected), "GUI CLI output was not captured on the expected stream");
        if (error)
            require(process.readAllStandardOutput().isEmpty(), "GUI CLI error was written to stdout");
        require(!QFile::exists(directory.filePath("config.json")), "GUI CLI initialized runtime configuration");
    };
    check_cli({"--help"}, 0, "--remote-host", false);
    check_cli({"--version"}, 0, "1.0.0", false);
    check_cli({"--remote-port", "invalid"}, 2, "Invalid --remote-port", true);
    check_cli({"--remote-port", "65536"}, 2, "Invalid --remote-port", true);
    check_cli({"--unknown-option"}, 2, "Unknown option", true);
    {
        QProcess smoke;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QT_VISUALIZATION_SCHEMA_REGRESSION", "1");
        smoke.setProcessEnvironment(environment);
#ifdef Q_OS_WIN
        smoke.start(QCoreApplication::applicationDirPath() + "/core_smoke.exe");
#else
        smoke.start(QCoreApplication::applicationDirPath() + "/core_smoke");
#endif
        require(smoke.waitForStarted(3000), "Schema smoke subprocess did not start");
        const bool finished = smoke.waitForFinished(10000);
        if (!finished) { smoke.kill(); smoke.waitForFinished(3000); }
        require(finished && smoke.exitStatus() == QProcess::NormalExit && smoke.exitCode() == 0,
                "Incompatible bootstrap / explicit recovery regression failed");
    }
    const auto path = directory.filePath("config.json");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot write config fixture");
    file.write(
        R"({"extension":{"keep":7},"core":{"future":true},"ui":{"camera_controls":{"future":{"keep":3}},"camera":{"future":8},"future":{"nested":9}},"wled_ar_server":{"future":10}})");
    file.close();
    ConfigManager manager(path);
    FileSaveApplicationConfig config;
    require(manager.load(config), "Cannot load config fixture");
    config.core.capture_source = 1;
    config.ui.show_spectrogram = false;
    require(manager.save(config), "Cannot save config fixture");
    require(file.open(QIODevice::ReadOnly), "Cannot read roundtrip config");
    const auto saved = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    require(saved.value("extension").toObject().value("keep").toInt() == 7, "Unknown top-level field lost");
    require(saved.value("core").toObject().value("future").toBool(), "Unknown core field lost");
    const auto ui = saved.value("ui").toObject();
    require(ui.value("future").toObject().value("nested").toInt() == 9, "Unknown UI object lost");
    require(ui.value("camera_controls").toObject().value("future").toObject().value("keep").toInt() == 3,
            "Unknown control field lost");
    require(saved.value("wled_ar_server").toObject().value("future").toInt() == 10, "Unknown WLED field lost");
    require(saved.value("core").toObject().value("capture_source").toInt() == 1, "Known config edit lost");
    {
        FileSaveApplicationConfig malformed;
        malformed.raw_document = QJsonObject{{"pipeline_graph", "future-format"}, {"scene_graph", QJsonArray{42}}};
        malformed.ui.show_spectrogram = false;
        require(manager.save(malformed), "Cannot save malformed schema prefs");
        require(file.open(QIODevice::ReadOnly), "Cannot read malformed schema prefs");
        const auto preserved = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        require(preserved["pipeline_graph"] == malformed.raw_document["pipeline_graph"] &&
                preserved["scene_graph"] == malformed.raw_document["scene_graph"],
                "Prefs normalized malformed schema fields destructively");
    }
    GuiState state;
    state.file_configs.core.run_analyser = false;
    state.file_configs.core.run_loopback = false;
    state.file_configs.ui.camera = QJsonObject{{"test", 42}};
    GuiPresenter presenter(state, test_fixture::synthetic_engine_provider());
    quint64 completed = 0;
    bool accepted = false;
    QObject::connect(&presenter, &GuiPresenter::graph_apply_finished, &app, [&](quint64 id, bool ok, const QString &) {
        completed = id;
        accepted = ok;
    });
    const auto valid = pipeline::GraphProgram::defaults(0, SpectrumMerge::Energy, 0);
    auto request = presenter.apply_pipeline_graph(valid);
    require(completed == 0, "Graph command completed synchronously");
    wait([&] { return completed == request; });
    require(accepted && state.file_configs.pipeline_graph == pipeline::canonical_graph_document(valid), "Accepted graph not reflected in GUI state");
    const auto active = state.file_configs.pipeline_graph;
    const auto draft = QJsonObject{{"version", 1}, {"nodes", QJsonArray{}}};
    state.file_configs.scene_graph = draft;
    request = presenter.apply_pipeline_graph(draft);
    wait([&] { return completed == request; });
    require(!accepted, "Invalid graph was accepted");
    require(state.file_configs.pipeline_graph == active, "Rejected draft replaced active graph");
    require(state.file_configs.scene_graph == draft, "Rejected draft was lost");
    require(state.file_configs.ui.camera.value("test").toInt() == 42, "Runtime reply modified view state");
    load_editor(state, valid);
    state.file_configs.core.spectrum_merge = SpectrumMerge::Maximum;
    const auto editor_before = state.data_flow_graph->save();
    presenter.apply_processing_settings();
    require(state.data_flow_graph->save() == editor_before, "Settings mutated draft before accepted");
    // An edit made while the request is in flight must survive its accepted reply.
    auto *merge_model = state.data_flow_graph->delegateModel<AudioNodeModel>(2);
    auto edited = merge_model->save(); edited["merge"] = 0; edited["user-note"] = "in flight";
    merge_model->load(edited);
    wait([&] { return !presenter.graph_request_pending(); });
    require(merge_model->save() == edited, "Accepted Settings lost concurrent draft edits");
    require(state.file_configs.scene_graph == draft, "Settings replaced the separate saved draft");
    {
        GuiState broken;
        broken.file_configs.core.run_analyser = broken.file_configs.core.run_loopback = false;
        broken.file_configs.scene_graph = draft;
        broken.file_configs.pipeline_graph = draft;
        load_editor(broken, draft);
        GuiPresenter fallback(broken, test_fixture::synthetic_engine_provider());
        int automatic_saves = 0;
        QObject::connect(&fallback, &GuiPresenter::on_config_updated, &fallback, [&] { ++automatic_saves; });
        fallback.initialize();
        wait([&] { return !fallback.graph_request_pending(); });
        require(automatic_saves == 0 && broken.file_configs.pipeline_graph == draft &&
                broken.file_configs.scene_graph == draft && !fallback.saved_graph_warning().isEmpty(),
                "Invalid version1 bootstrap replaced saved schemas or requested autosave");
        broken.file_configs.core.spectrum_merge = SpectrumMerge::Maximum;
        fallback.apply_processing_settings();
        wait([&] { return !fallback.graph_request_pending(); });
        require(broken.file_configs.pipeline_graph == draft && broken.file_configs.scene_graph == draft &&
                broken.data_flow_graph->allNodeIds().empty(), "Processing prefs replaced invalid version1 schemas");
    }
    {
        pipeline::AudioPipeline engine(test_fixture::synthetic_capture_factory());
        pipeline::EngineServer readonly(engine);
        require(readonly.listen(QHostAddress::LocalHost, 0, false), "Cannot start read-only peer");
        GuiState remote_state; load_editor(remote_state, valid);
        remote_state.file_configs.scene_graph = draft;
        remote_state.file_configs.pipeline_graph = valid;
        remote_state.file_configs.pipeline_graph["saved-local"] = true;
        const auto saved_local = remote_state.file_configs.pipeline_graph;
        GuiPresenter remote(remote_state, test_fixture::synthetic_engine_provider(), "127.0.0.1", readonly.port());
        wait([&] { return remote.client().snapshot().epoch != 0; });
        require(remote_state.file_configs.raw_document["devices"].toObject()["local"].toObject()["pipeline_graph"].toObject() == saved_local, "Handshake lost saved local pipeline");
        const auto remote_draft=remote_state.file_configs.scene_graph;
        const auto remote_working=remote_state.file_configs.pipeline_graph;
        const auto before = remote_state.data_flow_graph->save();
        QString rejection;
        QObject::connect(&remote, &GuiPresenter::command_rejected, &remote,
                         [&](quint64, QString error) { rejection = error; });
        remote_state.file_configs.core.spectrum_merge = SpectrumMerge::Maximum;
        remote.apply_processing_settings();
        require(remote_state.data_flow_graph->save() == before, "Remote Settings mutated draft synchronously");
        wait([&] { return !remote.graph_request_pending(); });
        require(rejection == "Server is read-only", "Read-only rejection not surfaced");
        require(remote_state.data_flow_graph->save() == before && remote_state.file_configs.scene_graph == remote_draft &&
                remote_state.file_configs.pipeline_graph == remote_working, "Rejected Settings changed saved/editor schemas");
        require(remote_state.file_configs.core.spectrum_merge == SpectrumMerge::Energy, "Rejected Settings did not refresh active values");
        pipeline::EngineServer writable(engine);
        require(writable.listen(QHostAddress::LocalHost, 0, true), "Cannot start writable peer");
        GuiState conflict_state; load_editor(conflict_state, valid);
        conflict_state.file_configs.scene_graph = draft;
        GuiPresenter conflict(conflict_state, test_fixture::synthetic_engine_provider(), "127.0.0.1", writable.port());
        wait([&] { return conflict.client().snapshot().epoch != 0; });
        QString conflict_error;
        QObject::connect(&conflict, &GuiPresenter::command_rejected, &conflict,
                         [&](quint64, QString error) { conflict_error = error; });
        const auto conflict_before = conflict_state.data_flow_graph->save();
        const auto conflict_draft=conflict_state.file_configs.scene_graph;
        conflict_state.file_configs.core.spectrum_merge = SpectrumMerge::Maximum;
        conflict.apply_processing_settings();
        conflict_state.file_configs.core.spectrum_merge = SpectrumMerge::Mean;
        conflict.apply_processing_settings(); // Both commands carry the same expected revision.
        wait([&] { return !conflict.graph_request_pending(); });
        require(conflict_error.isEmpty(), "Coalesced Settings conflicted with its own in-flight request");
        require(conflict_state.data_flow_graph->delegateModel<AudioNodeModel>(2)->save()["merge"].toInt()==0 &&
                conflict_state.file_configs.scene_graph == conflict_draft, "Coalescing did not update matching editor or replaced saved draft");
        require(conflict_state.file_configs.core.spectrum_merge == SpectrumMerge::Mean,
                "Coalesced Settings did not apply newest values");
    }
    {
        DelayedEnginePeer peer;
        require(peer.listen(QHostAddress::LocalHost, 0), "Cannot start delayed peer");
        GuiState delayed_state; load_editor(delayed_state, valid);
        delayed_state.file_configs.scene_graph = draft;
        const auto delayed_id=QString("127.0.0.1:")+QString::number(peer.serverPort());
        delayed_state.file_configs.raw_document["devices"]=QJsonObject{{delayed_id,QJsonObject{{"scene_graph",draft},{"pipeline_graph",valid}}}};
        GuiPresenter delayed(delayed_state, test_fixture::synthetic_engine_provider(), "127.0.0.1", peer.serverPort());
        wait([&] { return delayed.client().snapshot().epoch != 0; });
        int finished = 0;
        QObject::connect(&delayed, &GuiPresenter::graph_apply_finished, &delayed,
                         [&](quint64, bool ok, QString) { require(ok, "Delayed command rejected"); ++finished; });
        delayed_state.file_configs.core.spectrum_merge = SpectrumMerge::Maximum;
        delayed.apply_processing_settings();
        delayed_state.file_configs.core.spectrum_merge = SpectrumMerge::Mean;
        delayed.apply_processing_settings();
        wait([&] { return peer.requests.size() == 1; });
        QThread::msleep(20); QCoreApplication::processEvents();
        require(peer.requests.size()==1,"More than one graph request in flight");
        peer.reply(peer.requests[0]);
        wait([&] { return peer.requests.size() == 2; });
        peer.reply(peer.requests[1]);
        wait([&] { return finished == 2; });
        auto *model = delayed_state.data_flow_graph->delegateModel<AudioNodeModel>(2);
        require(model->save()["merge"].toInt() == 0, "Newest accepted Settings not reflected in draft");
        auto parallel = model->save(); parallel["merge"] = 2; model->load(parallel);
        peer.reply(peer.requests[0]);
        QThread::msleep(10);QCoreApplication::processEvents();
        require(finished==2,"Duplicate old reply emitted another completion");
        require(model->save() == parallel, "Out-of-order reply erased a later editor change");
        require(delayed_state.file_configs.pipeline_graph == peer.requests[1].graph &&
                delayed_state.file_configs.core.spectrum_merge == SpectrumMerge::Mean,
                "Out-of-order reply regressed active/saved pipeline");
        require(delayed_state.file_configs.scene_graph == draft, "Remote replies replaced saved draft");
    }
    {
        pipeline::AudioPipeline engine(test_fixture::synthetic_capture_factory());pipeline::EngineServer server(engine);
        require(server.listen(QHostAddress::LocalHost,0,false),"Device server");
        GuiState isolated;isolated.file_configs.core.run_analyser=isolated.file_configs.core.run_loopback=false;
        isolated.file_configs.scene_graph=valid;isolated.file_configs.scene_graph["local-extension"]=42;
        isolated.file_configs.ui.pipeline_presets=QJsonArray{QJsonObject{{"id","local-preset"}}};
        isolated.file_configs.ui.editor_view={{"zoom",.5}};GuiPresenter selector(isolated, test_fixture::synthetic_engine_provider());
        selector.initialize();wait([&]{return !selector.graph_request_pending();});
        const auto local_draft=isolated.file_configs.scene_graph;int selected=0;
        QObject::connect(&selector,&GuiPresenter::selected_device_changed,&selector,[&](auto,auto){++selected;});
        selector.select_device("remote-uuid","127.0.0.1",server.port());
        wait([&]{return selector.client().snapshot().epoch!=0 && selected>=2;});
        require(!selector.graph_request_pending(),"Device selection automatically applied remote graph");
        require(isolated.file_configs.ui.pipeline_presets.isEmpty(),"Local presets leaked to remote");
        isolated.file_configs.scene_graph["remote-extension"]=99;
        selector.select_device("local",{},0);
        require(isolated.file_configs.scene_graph==local_draft && isolated.file_configs.ui.pipeline_presets.size()==1 && isolated.file_configs.ui.editor_view["zoom"].toDouble()==.5,"Device drafts/presets/view settings mixed");
        require(isolated.file_configs.raw_document["devices"].toObject()["remote-uuid"].toObject()["scene_graph"].toObject()["remote-extension"].toInt()==99,"Remote draft lost on switch");
    }
    {
        GuiState transitions;
        transitions.file_configs.core.run_analyser = transitions.file_configs.core.run_loopback = false;
        transitions.file_configs.pipeline_graph = valid;
        auto local_provider = test_fixture::synthetic_engine_provider();
        GuiPresenter local(transitions, local_provider);
        local.initialize();
        wait([&] { return !local.graph_request_pending(); });
        auto *engine = local_provider->local_engine();
        QString source_error;
        local.apply_graph_and_start(pipeline::GraphProgram::test_signal_defaults());
        wait([&] { return !local.graph_request_pending() &&
                          engine->current_status("Capture").state == pipeline::State::Running &&
                          engine->current_status("Analyzer").state == pipeline::State::Running; });
        local.apply_graph_and_start(valid);
        wait([&] { return !local.graph_request_pending() &&
                          engine->current_status("Analyzer").state == pipeline::State::Running &&
                          pipeline::GraphProgram::compile(engine->graph(), source_error)->capture_source() == 0; });
        local.stop_loopback(); local.stop_analyzer();
        wait([&] { return engine->current_status("Capture").state == pipeline::State::Stopped &&
                          engine->current_status("Analyzer").state == pipeline::State::Stopped; });
        local.apply_graph_and_start(valid);
        wait([&] { return !local.graph_request_pending() &&
                          engine->current_status("Analyzer").state == pipeline::State::Running &&
                          engine->current_status("Capture").state != pipeline::State::Stopped; });
        local.stop_loopback(); local.stop_analyzer();
        wait([&] { return engine->current_status("Capture").state == pipeline::State::Stopped &&
                          engine->current_status("Analyzer").state == pipeline::State::Stopped; });
    }
    {
        DelayedEnginePeer peer;
        require(peer.listen(QHostAddress::LocalHost, 0), "Cannot start intent peer");
        GuiState intent_state;
        GuiPresenter intent(intent_state, test_fixture::synthetic_engine_provider(), "127.0.0.1", peer.serverPort());
        intent.initialize();
        wait([&] { return intent.client().snapshot().epoch != 0; });
        auto starts = [&] {
            return std::count(peer.commands.begin(), peer.commands.end(), pipeline::EngineCommand::StartAnalyzer) +
                   std::count(peer.commands.begin(), peer.commands.end(), pipeline::EngineCommand::StartCapture);
        };
        require(starts() == 0, "Remote initialization started processing implicitly");
        intent.apply_graph_and_start(valid);
        wait([&] { return peer.requests.size() == 1; });
        require(starts() == 0, "Default started before its graph ACK");
        peer.reply(peer.requests[0]);
        wait([&] { return starts() == 2; });
        intent.apply_pipeline_graph(valid);
        wait([&] { return peer.requests.size() == 2; });
        peer.reply(peer.requests[1]);
        wait([&] { return !intent.graph_request_pending(); });
        require(starts() == 2, "Ordinary Apply started processing");
        intent.apply_graph_and_start(valid);
        wait([&] { return peer.requests.size() == 3; });
        peer.reply(peer.requests[2], false);
        wait([&] { return !intent.graph_request_pending(); });
        require(starts() == 2, "Rejected Default started processing");
        intent.apply_graph_and_start(valid);
        intent.apply_pipeline_graph(valid);
        wait([&] { return peer.requests.size() == 4; });
        peer.reply(peer.requests[3]);
        wait([&] { return peer.requests.size() == 5; });
        require(starts() == 2, "Superseded Default started processing");
        peer.reply(peer.requests[4]);
        wait([&] { return !intent.graph_request_pending(); });
        require(starts() == 2, "Replacement inherited stale Default start intent");
        intent.apply_graph_and_start(valid);
        wait([&] { return peer.requests.size() == 6; });
        intent.stop_loopback();
        peer.reply(peer.requests[5]);
        wait([&] { return !intent.graph_request_pending(); });
        require(starts() == 2, "Late graph ACK ignored explicit Stop");
        intent.apply_graph_and_start(valid);
        wait([&] { return peer.requests.size() == 7; });
        intent.select_device(intent.local_device_id(), {}, 0);
        peer.reply(peer.requests[6]);
        QThread::msleep(20); QCoreApplication::processEvents();
        require(starts() == 2, "Old device ACK started an inactive engine");
    }
    return 0;
}
