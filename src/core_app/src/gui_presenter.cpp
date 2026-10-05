#include "core_app/gui_presenter.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core/utils.h"
#include "core/performance.h"
#include <QJsonArray>

GuiPresenter::GuiPresenter(GuiState &state, const QString &remote_host, quint16 remote_port, QObject *parent)
    : QObject(parent), _state(state) {
    _saved_draft_unsupported = (_state.file_configs.raw_document.contains("scene_graph") ||
        !_state.file_configs.scene_graph.isEmpty()) && _state.file_configs.scene_graph.value("version").toInt(-1) != 1;
    QString saved_error;
    _preserve_saved_pipeline = (_state.file_configs.raw_document.contains("pipeline_graph") ||
        !_state.file_configs.pipeline_graph.isEmpty()) &&
        !pipeline::GraphProgram::compile(_state.file_configs.pipeline_graph, saved_error);
    if (_preserve_saved_pipeline)
        _saved_graph_warning = "Saved pipeline is invalid: " + saved_error +
            ". The saved document is preserved until an explicit pipeline Apply.";
    _client = remote_host.isEmpty() ? std::make_unique<pipeline::EngineClient>()
                                    : std::make_unique<pipeline::EngineClient>(remote_host, remote_port);
    _client->set_snapshot_handler([this](const auto &snapshot) { accept_snapshot(snapshot); });
    _client->set_reply_handler([this](const auto &reply) {
        if (!reply.accepted)
            emit command_rejected(reply.request_id, reply.error);
        const auto it = _pending_graphs.find(reply.request_id);
        if (it == _pending_graphs.end())
            return;
        const auto pending = it.value();
        _pending_graphs.erase(it);
        if (reply.accepted) {
            // EngineClient normalizes old replies to its newest snapshot. An old
            // settings reply must not patch the draft using obsolete parameters.
            if (pending.settings && !_saved_draft_unsupported && reply.request_id == _latest_settings_request &&
                pending.document == reply.snapshot.graph && _state.data_flow_graph) {
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
        emit graph_apply_finished(reply.request_id, reply.accepted, reply.error);
    });
    _client->set_view_handler([this](const auto &frame) {
        core::perf::Scope consume(core::perf::Stage::GuiSpectrumConsume);
        const auto now = core::perf::now();
        // Clock domains differ across hosts; queue latency is measurable locally.
        if (!_remote && frame.ready_timestamp_ns && now >= frame.ready_timestamp_ns)
            core::perf::record(core::perf::Stage::GuiSpectrumQueue, frame.ready_timestamp_ns,
                               now - frame.ready_timestamp_ns);
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
    _client->set_audio_handler([this](const auto &frame) {
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
    _remote = !remote_host.isEmpty();
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
        _client->connect_remote();
        return;
    }
    update_subscription();
    auto active = _state.file_configs.pipeline_graph;
    if (active.isEmpty() && !_state.file_configs.raw_document.contains("pipeline_graph"))
        active = _state.file_configs.scene_graph;
    QString error;
    if (!pipeline::GraphProgram::compile(active, error)) {
        _preserve_saved_pipeline = true;
        _saved_graph_warning = "Saved pipeline was not loaded: " + error +
            ". A temporary default is running; saved schemas are preserved. Use Default pipeline and Apply to recover.";
        emit saved_graph_warning_changed(_saved_graph_warning);
        active = pipeline::GraphProgram::defaults(_state.file_configs.core.spectrum_channel,
                                                  _state.file_configs.core.spectrum_merge,
                                                  _state.file_configs.core.capture_source);
    }
    PendingGraph bootstrap;
    bootstrap.document = active;
    bootstrap.bootstrap = true;
    submit_graph(std::move(bootstrap));
    if (_state.file_configs.core.run_analyser)
        start_analyzer();
    if (_state.file_configs.core.run_loopback)
        start_loopback();
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
    if (_visible_views) mask = *_visible_views;
    if (_subscription == mask)
        return;
    _subscription = mask;
    pipeline::EngineCommand command;
    command.kind = pipeline::EngineCommand::Subscribe;
    command.subscriptions = mask;
    _client->submit(command);
}
void GuiPresenter::set_visible_views(quint32 mask) {
    _visible_views = mask; update_subscription();
}
void GuiPresenter::accept_snapshot(const pipeline::EngineSnapshot &snapshot) {
    if (_remote && _subscription_epoch != snapshot.epoch) {
        _subscription_epoch = snapshot.epoch;
        _subscription = std::numeric_limits<quint32>::max();
        update_subscription();
    }
    if (!snapshot.graph.isEmpty()) {
        _active_graph = snapshot.graph;
        _sync_processing_config();
        emit processing_state_changed();
    }
    _window = snapshot.window;
    _frequency_step = snapshot.frequency_step;
    for (auto it = snapshot.statuses.begin(); it != snapshot.statuses.end(); ++it) {
        emit component_status_changed(it.key(), static_cast<int>(it.value().state), it.value().error);
        const bool running = it.value().state == pipeline::State::Running;
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
quint64 GuiPresenter::submit_graph(PendingGraph request) {
    pipeline::EngineCommand command;
    command.kind = pipeline::EngineCommand::ApplyGraph;
    command.graph = request.document;
    if (_remote)
        command.expected_revision = _client->snapshot().revision;
    const auto id = _client->submit(command);
    _pending_graphs.insert(id, std::move(request));
    return id;
}
void GuiPresenter::start_analyzer() {
    pipeline::EngineCommand c;
    c.kind = pipeline::EngineCommand::StartAnalyzer;
    _client->submit(c);
}
void GuiPresenter::stop_analyzer() {
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
    if (channel<0 || channel>2 || channel==_state.file_configs.core.spectrum_channel) return;
    _state.file_configs.core.spectrum_channel=channel;
    apply_processing_settings();
}
void GuiPresenter::set_spectrum_merge(int merge) {
    if (merge<0 || merge>2 || merge==static_cast<int>(_state.file_configs.core.spectrum_merge)) return;
    _state.file_configs.core.spectrum_merge=static_cast<SpectrumMerge>(merge);
    apply_processing_settings();
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
    draft_document["version"] = 1;
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
    bool merge_found = false;
    for (const auto &data : nodes) {
        if (data["model-name"] == "AudioSource")
            _state.file_configs.core.capture_source = data.value("source").toInt(0);
        if (data["model-name"] == "FFTView")
            _state.file_configs.core.spectrum_channel = data.value("channel").toInt(0);
        if (!merge_found && data["model-name"] == "SpectrumMerge") {
            _state.file_configs.core.spectrum_merge = static_cast<SpectrumMerge>(data.value("merge").toInt(2));
            merge_found = true;
        }
    }
}
