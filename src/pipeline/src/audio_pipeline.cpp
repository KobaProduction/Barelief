#include "core/performance.h"
#include "pipeline/audio_pipeline.h"
#include "core/latest_delivery.h"
#include <QMetaObject>
#include <chrono>
#include <algorithm>
#include <exception>
#include <QJsonArray>
namespace pipeline {
audio::loopback::WASAPILoopback *legacy_wasapi_capture(IAudioSource *source);
AudioPipeline::AudioPipeline() : AudioPipeline(default_audio_source_factory()) {}
AudioPipeline::AudioPipeline(AudioSourceFactory source_factory) : _source_factory(std::move(source_factory)) {
    if (!_source_factory) _source_factory = default_audio_source_factory();
    QString error;
    _graph = GraphProgram::compile(GraphProgram::defaults(), error);
    _runtime = new QObject;
    _runtime->moveToThread(&_thread);
    QObject::connect(&_thread, &QThread::finished, _runtime, &QObject::deleteLater);
    _wled_delivery = std::make_shared<LatestDelivery<WledFrame>>(_runtime, [this](WledFrame frame) {
        core::perf::duration(core::perf::Stage::WledQueue, core::perf::now() - frame.enqueued_ns);
        if (_wled && frame.generation == _network_generation.load() && frame.graph_revision == graph_revision())
            _wled->update(frame.mel, frame.hz, frame.magnitude);
    });
    _thread.setObjectName("AudioPipeline");
    _thread.start();
}
AudioPipeline::~AudioPipeline() {
    stop_capture();
    stop_analyzer();
    stop_wled();
    _thread.quit();
    _thread.wait();
}
void AudioPipeline::command(std::function<void()> operation) const {
    if (QThread::currentThread() == &_thread)
        operation();
    else {
        std::exception_ptr error;
        QMetaObject::invokeMethod(
            _runtime,
            [&operation, &error] {
                try {
                    operation();
                } catch (...) {
                    error = std::current_exception();
                }
            },
            Qt::BlockingQueuedConnection);
        if (error)
            std::rethrow_exception(error);
    }
}
void AudioPipeline::status(QString component, State state, QString error) {
    ComponentStatus value;
    {
        std::lock_guard lock(_status_mutex);
        value = {state, error, ++_status_revision};
        _statuses[component] = value;
    }
    std::function<void(QString, ComponentStatus)> handler;
    {
        std::lock_guard lock(_callback_mutex);
        handler = _status_handler;
    }
    if (handler)
        handler(component, value);
}
ComponentStatus AudioPipeline::current_status(const QString &component) const {
    std::lock_guard lock(_status_mutex);
    return _statuses.value(component);
}
void AudioPipeline::start_analyzer() {
    command([this] {
        std::unique_lock lock(_mutex);
        if (_analyzer && _analyzer->is_running())
            return;
        _analyzer.reset();
        const auto generation = ++_analyzer_generation;
        _history_snapshot.store(nullptr);
        _analyzer = std::make_unique<Analyzer>();
        _analyzer->set_mel_enabled(false);
        {
            std::lock_guard history_lock(_history_mutex);
            _history = std::make_unique<Spectrogram>(400);
            _processed = 0;
        }
        _analyzer->update_sample_rate(_rate);
        _analyzer->set_frame_handler([this, generation](const auto &input) {
            auto frame = input;
            frame.generation = generation;
            if (generation == _analyzer_generation.load())
                process(frame);
        });
        _analyzer->set_lifecycle_handler([this, generation](bool ready, uint64_t result, const std::string &error) {
            if (generation != _analyzer_generation.load())
                return;
            status("Analyzer",
                   ready    ? State::Running
                   : result ? State::Failed
                            : State::Stopped,
                   result ? (error.empty() ? "Spectrum processing failed" : QString::fromStdString(error)) : QString{});
        });
        lock.unlock();
        status("Analyzer", State::Starting);
        try {
            _analyzer->start();
        } catch (const std::exception &error) {
            status("Analyzer", State::Failed, error.what());
            throw;
        }
    });
}
void AudioPipeline::stop_analyzer() {
    command([this] {
        ++_analyzer_generation;
        std::unique_ptr<Analyzer> retiring;
        {
            std::lock_guard lock(_mutex);
            retiring = std::move(_analyzer);
        }
        retiring.reset();
        {
            std::lock_guard lock(_history_mutex);
            _history.reset();
        }
        _history_snapshot.store(nullptr);
        status("Analyzer", State::Stopped);
    });
}
void AudioPipeline::feed(const AudioFrame &frame) {
    core::perf::Scope trace(core::perf::Stage::Feed);
    const auto waiting = core::perf::now();
    std::lock_guard lock(_mutex);
    core::perf::record(core::perf::Stage::FeedLock, waiting, core::perf::now() - waiting);
    if (!frame.sample_rate || frame.left.size() != frame.right.size())
        return;
    _rate = frame.sample_rate;
    if (_analyzer && frame.discontinuity)
        _analyzer->reset_samples();
    if (_analyzer) {
        _analyzer->update_sample_rate(_rate);
        _analyzer->add_samples(frame.left, frame.right, frame.timestamp_ns);
    }
}
void AudioPipeline::start_capture() {
    command([this] {
        if (_capture && _capture->is_running())
            return;
        const auto generation = ++_capture_generation;
        int source;
        {
            std::lock_guard lock(_mutex);
            source = _graph->capture_source();
        }
        if (_capture) { _capture->stop(); _capture.reset(); }
        try { _capture = _source_factory(static_cast<AudioSourceKind>(source)); }
        catch (const std::exception &error) { status("Capture", State::Failed, error.what()); throw; }
        if (!_capture) { status("Capture", State::Failed, "Audio source factory returned no source"); return; }
        _capture->set_status_handler([this, generation](bool running, QString error) {
            if (generation != _capture_generation.load())
                return;
            status("Capture", running ? State::Running : State::Recovering, std::move(error));
        });
        _capture->set_lifecycle_handler([this, generation](bool ready, uint64_t result, QString error) {
            if (!ready && result && generation == _capture_generation.load())
                status("Capture", State::Failed, std::move(error));
        });
        _capture->set_audio_handler([this, generation](AudioFrame frame) {
            core::perf::Scope trace(core::perf::Stage::CaptureDispatch);
            if (generation != _capture_generation.load() || !frame.sample_rate || frame.left.size() != frame.right.size()) return;
            frame.generation = generation;
            frame.sequence = ++_audio_sequence;
            if (!frame.timestamp_ns) frame.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            feed(frame);
            std::function<void(AudioFrame)> handler;
            {
                std::lock_guard lock(_callback_mutex);
                handler = _audio_handler;
            }
            if (handler)
                handler(std::move(frame));
        });
        status("Capture", State::Starting);
        try { _capture->start(); }
        catch (const std::exception &error) { _capture->stop(); _capture.reset(); status("Capture", State::Failed, error.what()); throw; }
    });
}
void AudioPipeline::stop_capture() {
    command([this] {
        ++_capture_generation;
        if (_capture) _capture->stop();
        _capture.reset();
        status("Capture", State::Stopped);
    });
}
void AudioPipeline::process(const SpectrumFrame &frame) {
    std::shared_ptr<const GraphProgram> program;
    uint64_t revision;
    {
        std::lock_guard lock(_mutex);
        program = _graph;
        revision = _graph_revision;
    }
    const auto output = [&] {
        core::perf::Scope trace(core::perf::Stage::Graph);
        return program->evaluate(frame, (_view_subscription.load() & 8) != 0);
    }();
    {
        std::lock_guard lock(_mutex);
        if (revision != _graph_revision)
            return;
        core::perf::Scope trace(core::perf::Stage::HistoryPush);
        std::lock_guard history_lock(_history_mutex);
        if (_history && frame.discontinuity) {
            _history->clear();
            _history_snapshot.store(nullptr);
        }
        if (_history && !output.history.empty()) {
            _history->set_history_size(output.history_size);
            _history->push(output.history);
        }
        ++_processed;
    }
    std::function<void(ViewFrame)> handler;
    {
        std::lock_guard lock(_callback_mutex);
        handler = _spectrum_handler;
    }
    if (!output.network.empty())
        publish_wled(output.network, output.peak_hz, output.magnitude, revision);
    if (handler) {
        core::perf::Scope trace(core::perf::Stage::SpectrumPublish);
        ViewFrame view{frame, output.fft, output.mel, output.previews, output.surface, revision, core::perf::now()};
        const auto subscriptions = _view_subscription.load();
        if (subscriptions & 16) {
            view.samples = frame.samples;
            view.window = frame.window;
            view.post_window = frame.post_window;
        }
        view.frequency_step = frame.frequency_step;
        if (!(subscriptions & 8))
            view.previews.clear();
        if (subscriptions & 4)
            view.history = latest_history();
        if (view.history && (view.history->graph_revision != revision ||
                             view.history->analyzer_generation != frame.generation))
            view.history.reset();
        handler(std::move(view));
    }
}
bool AudioPipeline::apply_graph(const QJsonObject &document, QString &error) {
    auto program = GraphProgram::compile(document, error);
    if (!program)
        return false;
    std::unique_lock lock(_mutex);
    const bool source_changed = _graph->capture_source() != program->capture_source();
    if (_graph->same_processing_as(*program)) {
        _graph = std::move(program);
        return true;
    }
    _graph = std::move(program);
    ++_graph_revision;
    ++_network_generation;
    _history_snapshot.store(nullptr);
    {
        std::lock_guard history_lock(_history_mutex);
        if (_history)
            _history->clear();
    }
    lock.unlock();
    if (source_changed)
        command([this] {
            const bool running = _capture && _capture->is_running();
            if (running)
                stop_capture();
            const bool analysing = analyzer() && analyzer()->is_running();
            if (analysing) {
                stop_analyzer();
                start_analyzer();
            }
            if (running)
                start_capture();
        });
    return true;
}
uint64_t AudioPipeline::graph_revision() const {
    std::lock_guard lock(_mutex);
    return _graph_revision;
}
bool AudioPipeline::audio_view_enabled() const {
    std::lock_guard lock(_mutex);
    return _graph->contains("AudioView");
}
QJsonObject AudioPipeline::graph() const {
    std::lock_guard lock(_mutex);
    return _graph->document();
}
void AudioPipeline::publish_wled(const std::vector<float> &mel, float hz, float magnitude, uint64_t revision) {
    _wled_delivery->publish({mel, hz, magnitude, _network_generation.load(), revision, core::perf::now()});
}
void AudioPipeline::start_wled(const QHostAddress &address, quint16 port, const QHostAddress &destination) {
    command([this, address, port, destination] {
        if (_wled)
            return;
        ++_network_generation;
        auto server = std::make_unique<WLEDAudioReactiveServer>(address, port, destination);
        if (!server->ready()) {
            status("WLED", State::Failed, server->error());
            return;
        }
        server->set_error_handler([this](QString error) { status("WLED", State::Failed, error); });
        server->start();
        _wled = std::move(server);
        status("WLED", State::Running);
    });
}
void AudioPipeline::stop_wled() {
    command([this] {
        ++_network_generation;
        _wled.reset();
        status("WLED", State::Stopped);
    });
}
void AudioPipeline::set_merge(SpectrumMerge mode) {
    auto document = graph();
    auto nodes = document.value("nodes").toArray();
    for (int i = 0; i < nodes.size(); ++i) {
        auto node = nodes[i].toObject();
        auto data = node.value("internal-data").toObject();
        if (data["model-name"] == "SpectrumMerge" || data["model-name"] == "FFTView")
            data["merge"] = static_cast<int>(mode);
        node["internal-data"] = data;
        nodes[i] = node;
    }
    document["nodes"] = nodes;
    QString error;
    apply_graph(document, error);
}
void AudioPipeline::set_display_channel(int channel) {
    auto document = graph();
    auto nodes = document.value("nodes").toArray();
    for (int i = 0; i < nodes.size(); ++i) {
        auto node = nodes[i].toObject();
        auto data = node.value("internal-data").toObject();
        if (data["model-name"] == "FFTView")
            data["channel"] = std::clamp(channel, 0, 2);
        node["internal-data"] = data;
        nodes[i] = node;
    }
    document["nodes"] = nodes;
    QString error;
    apply_graph(document, error);
}
void AudioPipeline::set_spectrum_handler(std::function<void(ViewFrame)> h) {
    std::lock_guard lock(_callback_mutex);
    _spectrum_handler = std::move(h);
}
void AudioPipeline::set_audio_handler(std::function<void(AudioFrame)> h) {
    std::lock_guard lock(_callback_mutex);
    _audio_handler = std::move(h);
}
void AudioPipeline::set_status_handler(std::function<void(QString, ComponentStatus)> h) {
    std::lock_guard lock(_callback_mutex);
    _status_handler = std::move(h);
}
std::vector<std::vector<float>> AudioPipeline::history() const {
    std::lock_guard lock(_history_mutex);
    return _history ? _history->get_vector_spectrogram() : std::vector<std::vector<float>>{};
}
std::shared_ptr<const HistorySnapshot> AudioPipeline::latest_history() {
    auto result = _history_snapshot.load();
    const auto now = core::perf::now();
    auto last = _history_requested_ns.load();
    if (now - last < 16000000 || !_history_requested_ns.compare_exchange_strong(last, now))
        return result;
    if (!_history_pending.exchange(true))
        QMetaObject::invokeMethod(
            _runtime,
            [this] {
                core::perf::Scope trace(core::perf::Stage::HistorySnapshot);
                auto snapshot = std::make_shared<HistorySnapshot>();
                {
                    std::lock_guard lock(_mutex);
                    std::lock_guard history_lock(_history_mutex);
                    snapshot->graph_revision = _graph_revision;
                    snapshot->analyzer_generation = _analyzer_generation.load();
                    if (_history)
                        snapshot->values = _history->get_vector_spectrogram();
                }
                _history_snapshot.store(std::move(snapshot));
                _history_pending = false;
            },
            Qt::QueuedConnection);
    return result;
}
size_t AudioPipeline::history_count() const {
    std::lock_guard lock(_history_mutex);
    return _history ? _history->filled_count() : 0;
}
uint64_t AudioPipeline::processed_frames() const {
    std::lock_guard lock(_history_mutex);
    return _processed;
}
Analyzer *AudioPipeline::analyzer() const {
    std::lock_guard lock(_mutex);
    return _analyzer.get();
}
Spectrogram *AudioPipeline::spectrogram() const {
    std::lock_guard lock(_history_mutex);
    return _history.get();
}
audio::loopback::WASAPILoopback *AudioPipeline::capture() const {
    audio::loopback::WASAPILoopback *result;
    command([&] { result = legacy_wasapi_capture(_capture.get()); });
    return result;
}
WLEDAudioReactiveServer *AudioPipeline::wled() const {
    WLEDAudioReactiveServer *result;
    command([&] { result = _wled.get(); });
    return result;
}
} // namespace pipeline
