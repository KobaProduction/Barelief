#include "core_app/gui_presenter.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core/utils.h"
#include "core/performance.h"
#include <QJsonArray>
#include "visualization/ui_text.h"

GuiPresenter::GuiPresenter(GuiState &state, const QString &remote_host, quint16 remote_port, QObject *parent)
    : QObject(parent), _state(state) {
    _local_device_id = _state.file_configs.raw_document.value("device").toObject().value("uuid").toString("local");
    _device_id = remote_host.isEmpty() ? _local_device_id : remote_host + ":" + QString::number(remote_port);
    _graph_id = _state.file_configs.pipeline_graph.value("graph_id").toString("main");
    _saved_draft_unsupported =
        (_state.file_configs.raw_document.contains("scene_graph") || !_state.file_configs.scene_graph.isEmpty()) &&
        (_state.file_configs.scene_graph.value("version").toInt(-1) != 1 &&
         _state.file_configs.scene_graph.value("version").toInt(-1) != 2);
    QString saved_error;
    _preserve_saved_pipeline = (_state.file_configs.raw_document.contains("pipeline_graph") ||
                                !_state.file_configs.pipeline_graph.isEmpty()) &&
                               !pipeline::GraphProgram::compile(_state.file_configs.pipeline_graph, saved_error);
    if (_preserve_saved_pipeline)
        _saved_graph_warning = "Saved pipeline is invalid: " + saved_error +
                               ". The saved document is preserved until an explicit pipeline Apply.";
    _client = remote_host.isEmpty() ? std::make_unique<pipeline::EngineClient>()
                                    : std::make_unique<pipeline::EngineClient>(remote_host, remote_port);
    _remote = !remote_host.isEmpty();
    _remote_endpoint = _remote ? QJsonObject{{"address", remote_host}, {"port", remote_port}} : QJsonObject{};
    if (_remote) {
        auto devices = _state.file_configs.raw_document.value("devices").toObject();
        if (!devices.contains(_local_device_id))
            devices[_local_device_id] = QJsonObject{{"scene_graph", _state.file_configs.scene_graph},
                                                    {"pipeline_graph", _state.file_configs.pipeline_graph},
                                                    {"presets", _state.file_configs.ui.pipeline_presets},
                                                    {"editor_view", _state.file_configs.ui.editor_view}};
        _state.file_configs.raw_document["devices"] = devices;
        const auto document = devices.value(_device_id).toObject();
        _load_selected_snapshot = document.isEmpty();
        _state.file_configs.scene_graph = document.value("scene_graph").toObject();
        _state.file_configs.pipeline_graph = document.value("pipeline_graph").toObject();
        _state.file_configs.ui.pipeline_presets = document.value("presets").toArray();
        _state.file_configs.ui.editor_view = document.value("editor_view").toObject();
    }
    bind_client();
}
void GuiPresenter::bind_client() {
    const auto selected = _device_id;
    _client->set_snapshot_handler([this, selected](const auto &snapshot) {
        if (selected == _device_id)
            accept_snapshot(snapshot);
    });
    _client->set_reply_handler([this, selected](const auto &reply) {
        if (selected != _device_id)
            return;
        if (!reply.accepted)
            emit command_rejected(reply.request_id, reply.error);
        const auto it = _pending_graphs.find(reply.request_id);
        if (it == _pending_graphs.end())
            return;
        const auto pending = it.value();
        QString applied_error;
        const auto requested_program = pipeline::GraphProgram::compile(pending.document, applied_error);
        const auto replied_program = pipeline::GraphProgram::compile(reply.snapshot.graph, applied_error);
        const bool current_epoch = (!pending.epoch || pending.epoch == reply.snapshot.epoch) &&
                                   reply.snapshot.graph_revision >= pending.graph_revision &&
                                   (!reply.accepted || (requested_program && replied_program &&
                                                        requested_program->same_processing_as(*replied_program)));
        _pending_graphs.erase(it);
        if (reply.accepted && current_epoch) {
            // EngineClient normalizes old replies to its newest snapshot. An old
            // settings reply must not patch the draft using obsolete parameters.
            if (pending.settings && !_saved_draft_unsupported && pending.logical_id == _latest_settings_request &&
                pipeline::canonical_graph_document(pending.document) ==
                    pipeline::canonical_graph_document(reply.snapshot.graph) &&
                _state.data_flow_graph) {
                for (const auto &patch : pending.patches) {
                    if (!_state.data_flow_graph->nodeExists(patch.id))
                        continue;
                    if (auto *model = _state.data_flow_graph->delegateModel<AudioNodeModel>(patch.id);
                        model && model->save() == patch.before) {
                        model->load(patch.after);
                        _state.editor_draft_modified = true;
                    }
                }
            }
            if (!pending.bootstrap) {
                if (!pending.settings || !_preserve_saved_pipeline)
                    _state.file_configs.pipeline_graph = reply.snapshot.graph;
                if (!pending.settings) {
                    _preserve_saved_pipeline = false;
                    _saved_draft_unsupported = false;
                    _saved_graph_warning.clear();
                    emit saved_graph_warning_changed({});
                }
                emit on_config_updated();
            }
        }
        const bool accepted = reply.accepted && current_epoch;
        const auto error = current_epoch ? reply.error : QString("Engine generation changed");
        bool matching_draft = true;
        if (_state.data_flow_graph) {
            auto draft = _state.data_flow_graph->save();
            draft["version"] = pending.document.value("version").toInt(2);
            const auto draft_program = pipeline::GraphProgram::compile(draft, applied_error);
            matching_draft = draft_program && replied_program && draft_program->same_processing_as(*replied_program);
        }
        if (!_queued_graph)
            set_apply_status(accepted ? (matching_draft
                                             ? ui::text("Data Flow applied")
                                             : ui::text("Draft changed; the last valid pipeline continues running"))
                                      : ui::text("Not applied: ") + error);
        // Start intent belongs to this accepted transaction and selected device.
        // A newer queued document supersedes the intent along with its graph.
        if (accepted && !_queued_graph) {
            if (!_remote && !pending.bootstrap && (pending.start_analyzer || pending.start_capture)) {
                if (pending.start_analyzer) _state.file_configs.core.run_analyser = true;
                if (pending.start_capture) _state.file_configs.core.run_loopback = true;
                emit on_config_updated();
            }
            if (pending.start_analyzer)
                start_analyzer();
            if (pending.start_capture)
                start_loopback();
        }
        emit graph_apply_finished(pending.logical_id, accepted, error);
        if (_queued_graph) {
            auto next = std::move(*_queued_graph);
            _queued_graph.reset();
            if (!current_epoch ||
                (_remote && (!reply.snapshot.epoch || reply.error.contains("Disconnected", Qt::CaseInsensitive))))
                emit graph_apply_finished(next.logical_id, false, "Graph application cancelled after connection loss");
            else
                dispatch_graph(std::move(next));
        }
    });
    _client->set_view_handler([this, selected](const auto &frame) {
        if (selected != _device_id)
            return;
        core::perf::Scope consume(core::perf::Stage::GuiSpectrumConsume);
        const auto now = core::perf::now();
        // Clock domains differ across hosts; queue latency is measurable locally.
        if (!_remote && frame.ready_timestamp_ns && now >= frame.ready_timestamp_ns)
            core::perf::record(core::perf::Stage::GuiSpectrumQueue, frame.ready_timestamp_ns,
                               now - frame.ready_timestamp_ns);
        if (frame.epoch != _client->snapshot().epoch || frame.graph_revision != _client->snapshot().graph_revision)
            return;
        emit node_views_ready(frame.views);
        _frequency_step = frame.frequency_step;
        _window = frame.window;
        emit window_signal_ready(frame.window_signal);
        emit window_function_ready(frame.window);
        emit windowed_signal_ready(frame.windowed_signal);
        emit amplitudes_ready(frame.fft);
        emit test_amplitudes_ready(frame.mel);
        emit pipeline_preview(frame.previews);
        if (frame.spectrogram && frame.history)
            emit spectrogram_updated(frame.history->values);
    });
    _client->set_audio_handler([this, selected](const auto &frame) {
        if (selected != _device_id)
            return;
        core::perf::Scope consume(core::perf::Stage::GuiAudioConsume);
        const auto now = core::perf::now();
        if (!_remote && frame.timestamp_ns > 0 && now >= static_cast<uint64_t>(frame.timestamp_ns))
            core::perf::record(core::perf::Stage::GuiAudioQueue, frame.timestamp_ns, now - frame.timestamp_ns);
        std::vector<float> samples(frame.left.size());
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = i < frame.right.size() ? (frame.left[i] + frame.right[i]) * 0.5f : frame.left[i];
        emit raw_signal_ready(samples);
        if (_sample_rate != frame.sample_rate) {
            _sample_rate = static_cast<uint32_t>(frame.sample_rate);
            emit sample_rate_changed(_sample_rate);
        }
    });
}

GuiPresenter::~GuiPresenter() = default;
void GuiPresenter::initialize() {
    if (_saved_draft_unsupported) {
        _saved_graph_warning = "Saved draft has an unsupported format and is preserved. "
                               "Use Default pipeline and Apply to replace it explicitly.";
    }
    if (!_saved_graph_warning.isEmpty())
        emit saved_graph_warning_changed(_saved_graph_warning);
    if (_remote) {
        emit selected_device_changed(_state.file_configs.scene_graph, _state.file_configs.ui.editor_view);
        _client->connect_remote();
        return;
    }
    QString network_error;
    if (!_client->configure_network(_state.file_configs.raw_document, &network_error)) {
        set_apply_status(ui::text("Not applied: ") + network_error);
        emit command_rejected(0, network_error);
    }
    update_subscription();
    auto active = _state.file_configs.pipeline_graph;
    if (active.isEmpty() && !_state.file_configs.raw_document.contains("pipeline_graph"))
        active = _state.file_configs.scene_graph;
    QString error;
    if (!pipeline::GraphProgram::compile(active, error)) {
        _preserve_saved_pipeline = true;
        _saved_graph_warning =
            "Saved pipeline was not loaded: " + error +
            ". A temporary default is running; saved schemas are preserved. Use Default pipeline and Apply to recover.";
        emit saved_graph_warning_changed(_saved_graph_warning);
        active = pipeline::GraphProgram::defaults(_state.file_configs.core.spectrum_channel,
                                                  _state.file_configs.core.spectrum_merge,
                                                  _state.file_configs.core.capture_source);
    }
    PendingGraph bootstrap;
    bootstrap.document = active;
    bootstrap.bootstrap = true;
    // Node enablement is authoritative. Apply starts enabled local sources;
    // deprecated persisted run flags must neither stop nor revive this graph.
    submit_graph(std::move(bootstrap));
    if (_state.file_configs.wled_ar_server.enabled)
        start_wled_audio_reactive_server();
}
void GuiPresenter::update_subscription() {
    if (_remote && !_client->snapshot().epoch)
        return;
    const auto &ui = _state.file_configs.ui;
    quint32 mask = pipeline::EngineCommand::Views | pipeline::EngineCommand::Previews;
    if (ui.show_raw_samples)
        mask |= pipeline::EngineCommand::Audio;
    if (ui.show_spectrogram)
        mask |= pipeline::EngineCommand::History;
    if (ui.show_window_signal || ui.show_window_function || ui.show_windowed_signal)
        mask |= pipeline::EngineCommand::Diagnostics;
    if (_visible_views)
        mask = *_visible_views;
    if (mask & pipeline::EngineCommand::Views)
        mask |= pipeline::EngineCommand::TypedViews;
    if (_subscription == mask)
        return;
    _subscription = mask;
    pipeline::EngineCommand command;
    command.kind = pipeline::EngineCommand::Subscribe;
    command.subscriptions = mask;
    _client->submit(command);
}
void GuiPresenter::set_visible_views(quint32 mask) {
    _visible_views = mask;
    update_subscription();
}
void GuiPresenter::accept_snapshot(const pipeline::EngineSnapshot &snapshot) {
    if (!snapshot.epoch) {
        emit views_cleared();
        set_apply_status(ui::text("Not applied: ") + ui::text("Connection lost"));
    }
    if (_load_selected_snapshot && snapshot.epoch && !snapshot.graph.isEmpty()) {
        _load_selected_snapshot = false;
        _state.file_configs.scene_graph = snapshot.graph;
        _state.file_configs.pipeline_graph = snapshot.graph;
        _preserve_saved_pipeline = false;
        _saved_draft_unsupported = false;
        emit selected_device_changed(snapshot.graph, _state.file_configs.ui.editor_view);
    }
    if (_remote && _subscription_epoch != snapshot.epoch) {
        _subscription_epoch = snapshot.epoch;
        _subscription = std::numeric_limits<quint32>::max();
        update_subscription();
    }
    if (!snapshot.graph.isEmpty()) {
        if (_active_graph != snapshot.graph) {
            _active_graph = snapshot.graph;
            _graph_id = snapshot.graph.value("graph_id").toString("main");
            // Compatible view/history state survives edits of processing links.
            emit active_graph_changed(snapshot.graph);
        }
        _sync_processing_config();
        emit processing_state_changed();
    }
    _window = snapshot.window;
    _frequency_step = snapshot.frequency_step;
    for (auto it = snapshot.statuses.begin(); it != snapshot.statuses.end(); ++it) {
        emit component_status_changed(it.key(), static_cast<int>(it.value().state), it.value().error);
        const bool running = it.value().state == pipeline::State::Running;
        if (!running && it.key() == "Connection")
            emit views_cleared();
        if (it.key() == "Capture") {
            if (running)
                emit loopback_started();
            else
                emit loopback_stopped();
        }
        if (it.key() == "Analyzer") {
            if (running)
                emit analyzer_started();
            else
                emit analyzer_stopped();
        }
    }
}
quint64 GuiPresenter::apply_pipeline_graph(const QJsonObject &document) {
    PendingGraph request;
    request.document = document;
    return submit_graph(std::move(request));
}
quint64 GuiPresenter::apply_graph_and_start(const QJsonObject &document) {
    PendingGraph request;
    request.document = document;
    request.start_analyzer = request.start_capture = true;
    return submit_graph(std::move(request));
}
void GuiPresenter::set_apply_status(QString status) {
    _apply_status = std::move(status);
    emit apply_status_changed(_apply_status);
}
quint64 GuiPresenter::submit_graph(PendingGraph request) {
    request.logical_id = ++_logical_request;
    const auto id = request.logical_id;
    if (!_pending_graphs.isEmpty()) {
        if (_queued_graph)
            emit graph_apply_finished(_queued_graph->logical_id, false, "Superseded by a newer draft");
        _queued_graph = std::move(request);
        set_apply_status(ui::text("Applying Data Flow…"));
    } else
        dispatch_graph(std::move(request));
    return id;
}
quint64 GuiPresenter::dispatch_graph(PendingGraph request) {
    pipeline::EngineCommand command;
    command.kind = pipeline::EngineCommand::ApplyGraph;
    command.graph = request.document;
    request.epoch = _client->snapshot().epoch;
    request.graph_revision = _client->snapshot().graph_revision;
    if (_remote)
        command.expected_revision = _client->snapshot().revision;
    const auto id = _client->submit(command);
    _pending_graphs.insert(id, std::move(request));
    set_apply_status(ui::text("Applying Data Flow…"));
    return id;
}
void GuiPresenter::start_analyzer() {
    pipeline::EngineCommand c;
    c.kind = pipeline::EngineCommand::StartAnalyzer;
    _client->submit(c);
}
void GuiPresenter::stop_analyzer() {
    for (auto &request : _pending_graphs)
        request.start_analyzer = request.start_capture = false;
    if (_queued_graph)
        _queued_graph->start_analyzer = _queued_graph->start_capture = false;
    emit explicit_stop_requested();
    emit views_cleared();
    pipeline::EngineCommand c;
    c.kind = pipeline::EngineCommand::StopAnalyzer;
    _client->submit(c);
}
void GuiPresenter::start_loopback() {
    pipeline::EngineCommand c;
    c.kind = pipeline::EngineCommand::StartCapture;
    _client->submit(c);
}
void GuiPresenter::stop_loopback() {
    for (auto &request : _pending_graphs)
        request.start_analyzer = request.start_capture = false;
    if (_queued_graph)
        _queued_graph->start_analyzer = _queued_graph->start_capture = false;
    emit explicit_stop_requested();
    emit views_cleared();
    pipeline::EngineCommand c;
    c.kind = pipeline::EngineCommand::StopCapture;
    _client->submit(c);
}
void GuiPresenter::start_wled_audio_reactive_server() {
    pipeline::EngineCommand c;
    c.kind = pipeline::EngineCommand::StartWled;
    c.interface_address = get_network_interface_address(_state.file_configs.wled_ar_server.network_interface_name);
    c.port = _state.file_configs.wled_ar_server.port;
    _client->submit(c);
}
void GuiPresenter::stop_wled_audio_reactive_server() {
    pipeline::EngineCommand c;
    c.kind = pipeline::EngineCommand::StopWled;
    _client->submit(c);
}
void GuiPresenter::restart_wled_audio_reactive_server() {
    stop_wled_audio_reactive_server();
    if (_state.file_configs.wled_ar_server.enabled)
        start_wled_audio_reactive_server();
}
void GuiPresenter::set_spectrum_channel(int channel) {
    if (channel < 0 || channel > 2 || channel == _state.file_configs.core.spectrum_channel)
        return;
    _state.file_configs.core.spectrum_channel = channel;
    for (const auto &value : _active_graph.value("nodes").toArray()) {
        auto n = value.toObject();
        if (n["internal-data"].toObject()["model-name"] == "FFTView") {
            set_node_parameters(n["id"].toInt(), {{"channel", channel}});
            return;
        }
    }
}
void GuiPresenter::set_spectrum_merge(int merge) {
    if (merge < 0 || merge > 2 || merge == static_cast<int>(_state.file_configs.core.spectrum_merge))
        return;
    _state.file_configs.core.spectrum_merge = static_cast<SpectrumMerge>(merge);
    for (const auto &value : _active_graph.value("nodes").toArray()) {
        auto n = value.toObject();
        if (n["internal-data"].toObject()["model-name"] == "SpectrumMerge") {
            set_node_parameters(n["id"].toInt(), {{"merge", merge}});
            return;
        }
    }
}
void GuiPresenter::apply_processing_settings() {
    auto document = _active_graph;
    if (document.isEmpty())
        document = _client->snapshot().graph;
    auto nodes = document.value("nodes").toArray();
    for (int i = 0; i < nodes.size(); ++i) {
        auto node = nodes[i].toObject();
        auto data = node.value("internal-data").toObject();
        if (data["model-name"] == "AudioSource")
            data["source"] = _state.file_configs.core.capture_source;
        if (data["model-name"] == "SpectrumMerge" || data["model-name"] == "FFTView")
            data["merge"] = static_cast<int>(_state.file_configs.core.spectrum_merge);
        if (data["model-name"] == "FFTView")
            data["channel"] = _state.file_configs.core.spectrum_channel;
        node["internal-data"] = data;
        nodes[i] = node;
    }
    document["nodes"] = nodes;
    PendingGraph request;
    request.document = document;
    request.settings = true;
    // Settings may follow the active pipeline only when the draft still denotes
    // that same processing graph. Independent/incomplete drafts stay untouched.
    QString draft_error;
    auto draft_document = _state.data_flow_graph ? _state.data_flow_graph->save() : QJsonObject{};
    draft_document["version"] = _active_graph.value("version").toInt(2);
    const auto draft_program = pipeline::GraphProgram::compile(draft_document, draft_error);
    const auto active_program = pipeline::GraphProgram::compile(_active_graph, draft_error);
    if (_state.data_flow_graph && !_saved_draft_unsupported && draft_program && active_program &&
        draft_program->same_processing_as(*active_program)) {
        for (auto id : _state.data_flow_graph->allNodeIds()) {
            if (auto *model = _state.data_flow_graph->delegateModel<AudioNodeModel>(id)) {
                auto data = model->save();
                if (model->name() == "AudioSource")
                    data["source"] = _state.file_configs.core.capture_source;
                if (model->name() == "SpectrumMerge" || model->name() == "FFTView")
                    data["merge"] = static_cast<int>(_state.file_configs.core.spectrum_merge);
                if (model->name() == "FFTView")
                    data["channel"] = _state.file_configs.core.spectrum_channel;
                if (data != model->save())
                    request.patches.push_back({static_cast<int>(id), model->save(), data});
            }
        }
    }
    _latest_settings_request = submit_graph(std::move(request));
}
void GuiPresenter::_sync_processing_config() {
    QMap<int, QJsonObject> nodes;
    for (const auto &value : _active_graph.value("nodes").toArray()) {
        const auto node = value.toObject();
        nodes.insert(node["id"].toInt(), node["internal-data"].toObject());
    }
    bool merge_found = false, channel_found = false, source_found = false;
    for (const auto &data : nodes) {
        if (!source_found && data["model-name"] == "AudioSource") {
            _state.file_configs.core.capture_source = data.value("source").toInt(0);
            source_found = true;
        }
        if (!channel_found && data["model-name"] == "FFTView") {
            _state.file_configs.core.spectrum_channel = data.value("channel").toInt(0);
            channel_found = true;
        }
        if (!merge_found && data["model-name"] == "SpectrumMerge") {
            _state.file_configs.core.spectrum_merge = static_cast<SpectrumMerge>(data.value("merge").toInt(2));
            merge_found = true;
        }
    }
}

quint64 GuiPresenter::set_node_parameters(int node_id, const QJsonObject &parameters) {
    auto document = _active_graph;
    if (_queued_graph && _queued_graph->settings)
        document = _queued_graph->document;
    else if (!_pending_graphs.isEmpty() && _pending_graphs.first().settings)
        document = _pending_graphs.first().document;
    auto nodes = document.value("nodes").toArray();
    for (int i = 0; i < nodes.size(); ++i) {
        auto node = nodes[i].toObject();
        if (node.value("id").toInt() != node_id)
            continue;
        auto data = node.value("internal-data").toObject();
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            data[it.key()] = it.value();
        node["internal-data"] = data;
        nodes[i] = node;
        document["nodes"] = nodes;
        PendingGraph request;
        request.document = document;
        request.settings = true;
        if (_state.data_flow_graph && _state.data_flow_graph->nodeExists(node_id)) {
            if (auto *model = _state.data_flow_graph->delegateModel<AudioNodeModel>(node_id)) {
                auto after = model->save();
                for (auto it = parameters.begin(); it != parameters.end(); ++it)
                    after[it.key()] = it.value();
                request.patches.push_back({node_id, model->save(), after});
            }
        }
        _latest_settings_request = submit_graph(std::move(request));
        return _latest_settings_request;
    }
    return 0;
}
void GuiPresenter::save_device_document() {
    auto devices = _state.file_configs.raw_document.value("devices").toObject();
    auto current = devices.value(_device_id).toObject();
    current["scene_graph"] = _state.file_configs.scene_graph;
    current["pipeline_graph"] = _state.file_configs.pipeline_graph;
    current["presets"] = _state.file_configs.ui.pipeline_presets;
    current["editor_view"] = _state.file_configs.ui.editor_view;
    devices[_device_id] = current;
    _state.file_configs.raw_document["devices"] = devices;
}
void GuiPresenter::select_device(const QString &uuid, const QString &host, quint16 port) {
    if (uuid.isEmpty() || uuid == _device_id)
        return;
    save_device_document();
    for (const auto &request : _pending_graphs)
        emit graph_apply_finished(request.logical_id, false, "Selected device changed");
    if (_queued_graph)
        emit graph_apply_finished(_queued_graph->logical_id, false, "Selected device changed");
    _pending_graphs.clear();
    _queued_graph.reset();
    pipeline::EngineCommand unsubscribe;
    unsubscribe.kind = pipeline::EngineCommand::Subscribe;
    unsubscribe.subscriptions = 0;
    _client->submit(unsubscribe);
    _inactive_clients[_device_id] = std::move(_client);
    _device_id = uuid;
    _remote = uuid != _local_device_id;
    _remote_endpoint = _remote ? QJsonObject{{"address", host}, {"port", port}} : QJsonObject{};
    auto found = _inactive_clients.find(uuid);
    if (found != _inactive_clients.end()) {
        _client = std::move(found->second);
        _inactive_clients.erase(found);
    } else {
        _client =
            _remote ? std::make_unique<pipeline::EngineClient>(host, port) : std::make_unique<pipeline::EngineClient>();
        bind_client();
    }
    const auto document = _state.file_configs.raw_document.value("devices").toObject().value(uuid).toObject();
    _state.file_configs.scene_graph = document.value("scene_graph").toObject();
    _state.file_configs.pipeline_graph = document.value("pipeline_graph").toObject();
    _state.file_configs.ui.pipeline_presets = document.value("presets").toArray();
    _state.file_configs.ui.editor_view = document.value("editor_view").toObject();
    _active_graph = {};
    _graph_id = "main";
    _subscription = std::numeric_limits<quint32>::max();
    _subscription_epoch = 0;
    _load_selected_snapshot = document.isEmpty();
    emit views_cleared();
    set_apply_status("Draft");
    emit selected_device_changed(_state.file_configs.scene_graph, _state.file_configs.ui.editor_view);
    if (_remote)
        _client->connect_remote();
    accept_snapshot(_client->snapshot());
    update_subscription();
    emit on_config_updated();
}
