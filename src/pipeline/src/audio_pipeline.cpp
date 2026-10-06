#include "core/performance.h"
#include "pipeline/audio_pipeline.h"
#include <QMetaObject>
#include <chrono>
#include <algorithm>
#include <exception>
#include <QJsonArray>
namespace pipeline {

AudioPipeline::AudioPipeline() : AudioPipeline(default_audio_source_factory()) {}
AudioPipeline::AudioPipeline(AudioSourceFactory source_factory) : _source_factory(std::move(source_factory)) {
    if (!_source_factory) _source_factory = default_audio_source_factory();
    QString error;
    _graph = GraphProgram::compile(GraphProgram::defaults(), error);
    _runtime = new QObject;
    _runtime->moveToThread(&_thread);
    QObject::connect(&_thread, &QThread::finished, _runtime, &QObject::deleteLater);
    _thread.setObjectName("AudioPipeline");
    _thread.start();
    command([this] {
        _history_timer = new QTimer(_runtime);
        _history_timer->setInterval(17);
        _history_timer->setTimerType(Qt::PreciseTimer);
        QObject::connect(_history_timer, &QTimer::timeout, _runtime, [this] {
            if (!_history_pending.exchange(false)) return;
            core::perf::Scope trace(core::perf::Stage::HistorySnapshot);
            auto snapshot = std::make_shared<HistorySnapshot>();
            {
                std::lock_guard lock(_mutex);
                std::lock_guard history_lock(_history_mutex);
                snapshot->graph_revision = _graph_revision;
                snapshot->analyzer_generation = _analyzer_generation.load();
                if (_history) snapshot->values = _history->get_vector_spectrogram();
            }
            _history_snapshot.store(std::move(snapshot));
        });
    });
}
AudioPipeline::~AudioPipeline() {
    { std::lock_guard lock(_callback_mutex);_spectrum_handler={};_audio_handler={};_status_handler={}; }
    { std::lock_guard lock(_typed_callback_mutex);_typed_handlers.clear(); }
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
        _analyzer_requested = true;
        _explicit_stop = false;
        std::unique_lock evaluation_lock(_evaluation_mutex);
        std::unique_lock lock(_mutex);
        if (_analyzer && _analyzer->is_running())
            return;
        _analyzer.reset();
        _typed_running=true;
        const auto generation = ++_analyzer_generation;
        _history_snapshot.store(nullptr);
        _history_pending = false;
        _history_requested_ns = 0;
        _analyzer = std::make_unique<Analyzer>();
        _analyzer->set_mel_enabled(false);
        _analyzer->set_pcm_only(true);
        _analyzer->set_stream_epoch(_epoch);
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
        int primary=INT_MAX;
        for(const auto &j:_graph->document().value("nodes").toArray()) {
            auto obj=j.toObject();auto kind=obj.value("internal-data").toObject().value("model-name").toString();
            if(obj.value("internal-data").toObject().value("enabled").toBool(true) && (kind=="AudioSource" || kind=="TestSignal")) primary=std::min(primary,obj.value("id").toInt());
        }
        _primary_source_id=primary==INT_MAX?-1:primary;
        _analyzer->set_ingress_serial(_ingress_serials.value(_primary_source_id));
        for(const auto &json:_graph->document().value("nodes").toArray()) {
            const auto obj=json.toObject();auto parameters=obj.value("internal-data").toObject();
            const auto kind=parameters.value("model-name").toString();
            if(kind!="AudioSource" && kind!="TestSignal") continue;
            if(!parameters.value("enabled").toBool(true)) continue;
            if(obj.value("id").toInt()==primary) continue;
            auto branch=std::make_unique<LocalBranch>();branch->id=obj.value("id").toInt();
            branch->source=kind=="TestSignal"?2:parameters.value("source").toInt();branch->parameters=parameters;
            branch->analyzer=std::make_unique<Analyzer>();branch->analyzer->set_pcm_only(true);branch->analyzer->set_stream_epoch(_epoch);branch->analyzer->set_ingress_serial(_ingress_serials.value(branch->id));branch->analyzer->set_mel_enabled(false);
            const auto id=branch->id;
            branch->analyzer->update_sample_rate(_rate);
            branch->analyzer->set_frame_handler([this,generation,id](const SpectrumFrame &input) {
                auto f=input;f.generation=generation;if(generation==_analyzer_generation.load()) process(f,id);
            });
            _local_branches.push_back(std::move(branch));
        }
        lock.unlock();
        evaluation_lock.unlock();
        status("Analyzer", State::Starting);
        try {
            _analyzer->start();
            for(const auto &branch:_local_branches) branch->analyzer->start();
            _history_timer->start();
            if(_capture_requested) { stop_capture();start_capture(); }
        } catch (const std::exception &error) {
            status("Analyzer", State::Failed, error.what());
            throw;
        }
    });
}
void AudioPipeline::stop_analyzer() {
    command([this] {
        _analyzer_requested = false;
        _explicit_stop = true;
        _history_timer->stop();
        ++_network_generation;
        { std::lock_guard lock(_wled_mailbox_mutex);_pending_wled.reset(); }
        _history_pending = false;
        ++_analyzer_generation;
        std::unique_ptr<Analyzer> retiring;
        std::vector<std::unique_ptr<LocalBranch>> retiring_branches;
        {
            std::lock_guard lock(_mutex);
            retiring = std::move(_analyzer);
            retiring_branches=std::move(_local_branches);
        }
        retiring.reset();
        for(auto &branch:retiring_branches) branch->analyzer->stop();
        retiring_branches.clear();
        {
            std::lock_guard lock(_history_mutex);
            _history.reset();
        }
        _history_snapshot.store(nullptr);
        {
            std::lock_guard evaluation_lock(_evaluation_mutex);_typed_running=false;++_epoch;_graph_context=GraphEvaluationContext{};invalidate_views();
            if(_wled_active.load()) publish_wled({},0,0,_graph_revision);
        }
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
        _capture_requested = true;
        _explicit_stop = false;
        if (_capture && _capture->is_running())
            return;
        const auto generation = ++_capture_generation;
        int source;
        QJsonObject parameters;
        {
            std::lock_guard lock(_mutex);
            source = _graph->capture_source();
            parameters = _graph->source_parameters();
        }
        if (_capture) { _capture->stop(); _capture.reset(); }
        if (source < 0) { status("Capture", State::Stopped); return; }
        try { _capture = _source_factory(static_cast<AudioSourceKind>(source)); }
        catch (const std::exception &error) { status("Capture", State::Failed, error.what()); throw; }
        if (!_capture) { status("Capture", State::Failed, "Audio source factory returned no source"); return; }
        try { _capture->configure(parameters); }
        catch (const std::exception &error) { _capture.reset(); status("Capture", State::Failed, error.what()); throw; }
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
        try {
            _capture->start();
            for(auto &owned:_local_branches) {
                auto *branch=owned.get();branch->capture=_source_factory(static_cast<AudioSourceKind>(branch->source));
                if(!branch->capture) continue;
                branch->capture->configure(branch->parameters);
                branch->capture->set_audio_handler([this,branch,generation](AudioFrame f) {
                    if(generation!=_capture_generation.load() || !f.sample_rate || f.left.size()!=f.right.size()) return;
                    f.sequence=++branch->sequence;
                    if(f.discontinuity) branch->analyzer->reset_samples();
                    branch->analyzer->update_sample_rate(f.sample_rate);branch->analyzer->add_samples(f.left,f.right,f.timestamp_ns);
                });
                branch->capture->start();
            }
        }
        catch (const std::exception &error) { _capture->stop(); _capture.reset(); status("Capture", State::Failed, error.what()); throw; }
    });
}
void AudioPipeline::stop_capture() {
    command([this] {
        _capture_requested = false;
        _explicit_stop = true;
        ++_capture_generation;
        ++_network_generation;
        { std::lock_guard lock(_wled_mailbox_mutex);_pending_wled.reset(); }
        if (_capture) _capture->stop();
        for(auto &branch:_local_branches) if(branch->capture) { branch->capture->stop();branch->capture.reset(); }
        _capture.reset();
        {
            std::lock_guard evaluation_lock(_evaluation_mutex);++_epoch;_graph_context=GraphEvaluationContext{};
            if(_analyzer) _analyzer->set_stream_epoch(_epoch);
            for(auto &branch:_local_branches) branch->analyzer->set_stream_epoch(_epoch);
            invalidate_views();
            if(_wled_active.load()) publish_wled({},0,0,_graph_revision);
        }
        status("Capture", State::Stopped);
    });
}
void AudioPipeline::process(const SpectrumFrame &frame, int ingress_node) {
    std::lock_guard evaluation_lock(_evaluation_mutex);
    if(ingress_node<0) ingress_node=_primary_source_id;
    if(frame.pipeline_epoch!=_epoch || frame.ingress_serial!=_ingress_serials.value(ingress_node)) return;
    std::shared_ptr<const GraphProgram> program;
    uint64_t revision;
    {
        std::lock_guard lock(_mutex);
        program = _graph;
        revision = _graph_revision;
    }
    const auto output = [&] {
        core::perf::Scope trace(core::perf::Stage::Graph);
        if (!frame.raw_window) return program->evaluate(frame, _graph_context, (_view_subscription.load() & 8) != 0);
        TypedFrame input; input.type=frame.raw_pcm?FrameType::PCM:FrameType::Window; input.left=frame.left;input.right=frame.right;
        input.sample_rate=frame.sample_rate;input.sequence=frame.sequence;input.generation=frame.generation;
        input.timestamp_ns=frame.timestamp_ns;input.discontinuity=frame.discontinuity;
        input.epoch=_epoch;input.graph_revision=revision;
        if(ingress_node>=0) return program->evaluate(ingress_node,input,_graph_context,(_view_subscription.load()&8)!=0);
        auto ids=program->ingress_nodes();
        for(auto id:ids) if(program->port_type(id)=="audio") return program->evaluate(id,input,_graph_context,(_view_subscription.load() & 8)!=0);
        return GraphOutput{};
    }();
    {
        std::lock_guard lock(_mutex);
        if (revision != _graph_revision)
            return;
        core::perf::Scope trace(core::perf::Stage::HistoryPush);
        std::lock_guard history_lock(_history_mutex);
        const bool history_active=std::any_of(output.history_events.begin(),output.history_events.end(),[&](const auto &e){return e.first==_compat_history_node;});
        if (_history && history_active && output.ports.value(_compat_history_node).discontinuity) {
            _history->clear(); _history_snapshot.store(nullptr);
        }
        if (_history && history_active) {
            _history->set_history_size(int(output.ports.value(_compat_history_node).history_capacity));
            for(const auto &[id,row]:output.history_events) if(id==_compat_history_node) _history->push(row);
        }
        _processed+=program->contains("Window")?output.windows_processed:1;
    }
    deliver_typed(output);
    std::function<void(ViewFrame)> handler;
    {
        core::perf::Scope trace(core::perf::Stage::CallbackSnapshot);
        std::lock_guard lock(_callback_mutex);
        handler = _spectrum_handler;
    }
    if (_wled_active.load()) {
        const auto light=output.ports.constFind(_compat_wled_node);
        if(light!=output.ports.cend() && !light->values.empty()) publish_wled(light->values,light->peak_hz,light->magnitude,revision);
    }
    if (handler) {
        core::perf::Scope trace(core::perf::Stage::SpectrumPublish);
        ViewFrame view{frame, output.fft, output.mel, output.previews, output.spectrogram, revision, core::perf::now()};
        view.epoch=_epoch;view.views=_views;
        if(frame.raw_window) {
            for(auto it=_views.begin();it!=_views.end();++it) if(it->type==FrameType::StereoSpectrum && !it->left.empty()) {
                view.spectrum.left=it->left;view.spectrum.right=it->right;view.spectrum.frequency_step=it->frequency_step;
                view.spectrum.sequence=it->sequence;view.spectrum.timestamp_ns=it->timestamp_ns;
                view.spectrum.input_timestamp_ns=it->timestamp_ns;
                view.fft=it->values;break;
            }
            for(auto it=_views.begin();it!=_views.end();++it) if(it->type==FrameType::Mel) {view.mel=it->values;break;}
        }
        const auto subscriptions = _view_subscription.load();
        if (subscriptions & 16) {
            view.window_signal = frame.window_signal;
            view.window = frame.window;
            view.windowed_signal = frame.windowed_signal;
            for(auto it=output.ports.begin();it!=output.ports.end();++it) {
                if(it->type==FrameType::Window) view.window_signal=it->left;
                if(it->type==FrameType::Windowed) view.windowed_signal=it->left;
            }
            if(!view.window_signal.empty()) { view.window.resize(view.window_signal.size());dsp_core::hann_window(view.window); }
        }
        view.frequency_step = view.spectrum.frequency_step;
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
    std::unique_lock evaluation_lock(_evaluation_mutex);
    std::unique_lock lock(_mutex);
    auto sources=[](const GraphProgram &p) {
        QJsonArray result;for(const auto &json:p.document().value("nodes").toArray()) {
            auto n=json.toObject();const auto kind=n.value("internal-data").toObject().value("model-name").toString();
            if(kind=="AudioSource" || kind=="TestSignal") {
                auto parameters=n.value("internal-data").toObject();parameters.remove("enabled");
                result.append(QJsonObject{{"id",n.value("id")},{"parameters",parameters}});
            }
        }
        return result;
    };
    bool source_changed=sources(*_graph)!=sources(*program);
    if(_analyzer_requested) for(const auto &json:program->document().value("nodes").toArray()) {
        const auto n=json.toObject();const auto parameters=n.value("internal-data").toObject();const auto kind=parameters["model-name"].toString();
        if(!parameters.value("enabled").toBool(true) || (kind!="AudioSource" && kind!="TestSignal")) continue;
        const int id=n["id"].toInt();
        if(id!=_primary_source_id && std::none_of(_local_branches.begin(),_local_branches.end(),[&](const auto &branch){return branch->id==id;})) source_changed=true;
    }
    if (_graph->same_processing_as(*program)) {
        _graph = std::move(program);
        _graph_context._program = _graph.get();
        const bool bootstrap=!_graph_applied && _graph->capture_source()>=0;
        _graph_applied=true;
        lock.unlock();evaluation_lock.unlock();
        if(bootstrap) command([this] {if(!_explicit_stop) {start_analyzer();start_capture();}});
        return true;
    }
    const auto previous_program=_graph;
    const auto previous_epoch=_epoch;
    _graph = std::move(program);
    _compat_history_node=-1;
    _compat_wled_node=-1;
    for(const auto &n:_graph->document().value("nodes").toArray()) {
        const auto obj=n.toObject();if(obj.value("internal-data").toObject().value("model-name").toString()=="Spectrogram") {
            const int id=obj.value("id").toInt();if(_compat_history_node<0 || id<_compat_history_node) _compat_history_node=id;
        }
        if(obj.value("internal-data").toObject().value("model-name").toString()=="WLED") {
            const int id=obj.value("id").toInt();if(_compat_wled_node<0 || id<_compat_wled_node) _compat_wled_node=id;
        }
    }
    ++_graph_revision;
    _accepted_after_ns=core::perf::now();
    ++_epoch;
    const auto reset_nodes=_graph->rebind(_graph_context,*previous_program,previous_epoch,_epoch);
    if(_analyzer) _analyzer->set_stream_epoch(_epoch,true);
    for(auto &branch:_local_branches) branch->analyzer->set_stream_epoch(_epoch,true);
    ++_network_generation;
    _history_snapshot.store(nullptr);
    {
        std::lock_guard history_lock(_history_mutex);
        if (_history && std::find(reset_nodes.begin(),reset_nodes.end(),_compat_history_node)!=reset_nodes.end())
            _history->clear();
    }
    lock.unlock();
    for(auto it=_views.begin();it!=_views.end();) {
        const auto nodes=_graph->document().value("nodes").toArray();
        const bool exists=std::any_of(nodes.begin(),nodes.end(),[&](const QJsonValue &n){return n.toObject().value("id").toInt()==it.key();});
        if(!exists) {it=_views.erase(it);continue;}
        if(std::find(reset_nodes.begin(),reset_nodes.end(),it.key())!=reset_nodes.end()) {
            const auto type=it->type;*it=TypedFrame{};it->type=type;it->available=false;
        }
        it->epoch=_epoch;it->graph_revision=_graph_revision;++it;
    }
    const auto blocked=_graph->disabled_output();
    if(_wled_active.load() && blocked.ports.contains(_compat_wled_node)) {
        {std::lock_guard mailbox_lock(_wled_mailbox_mutex);_pending_wled.reset();}
        publish_wled({},0,0,_graph_revision);
    }
    _graph_applied=true;
    auto retained_context=_graph_context;
    auto retained_views=_views;
    const auto applied_epoch=_epoch;
    const auto applied_revision=_graph_revision;
    evaluation_lock.unlock();
    command([this,source_changed,retained_context=std::move(retained_context),retained_views=std::move(retained_views),applied_epoch,applied_revision]() mutable {
            const bool stopped=_explicit_stop;
            const bool running = _capture_requested;
            const bool analysing = _analyzer_requested;
            if (source_changed && running)
                stop_capture();
            if (source_changed && analysing) {
                stop_analyzer();
                start_analyzer();
            }
            if (source_changed && running)
                start_capture();
            _explicit_stop=stopped;
            bool local=false;{std::lock_guard graph_lock(_mutex);local=_graph->capture_source()>=0;}
            if(local && !stopped && !running && !analysing) {start_analyzer();start_capture();}
            // Keep compatible histories across physical producer reconfiguration.
            if(source_changed) {
                std::lock_guard evaluation_lock(_evaluation_mutex);
                // Lifecycle helpers clear their legacy global snapshots. Restore
                // only the compatible states retained by this Apply transaction.
                if(_graph_revision==applied_revision && _epoch!=applied_epoch) {
                    _graph_context=std::move(retained_context);
                    for(auto &state:_graph_context._states) {
                        const auto nodes=_graph->document().value("nodes").toArray();
                        const bool local_clock=std::any_of(nodes.begin(),nodes.end(),[&](const QJsonValue &json) {
                            const auto n=json.toObject();const auto kind=n["internal-data"].toObject()["model-name"].toString();
                            return n["id"].toInt()==state.clock_ingress && (kind=="AudioSource" || kind=="TestSignal");
                        });
                        if(!local_clock || state.producer_epoch!=applied_epoch) continue;
                        state.producer_epoch=_epoch;state.generation=_analyzer_generation.load();
                        state.input_template.producer_epoch=_epoch;state.input_template.epoch=_epoch;
                        state.input_template.generation=state.generation;
                        state.sequence=0;
                    }
                    _views=std::move(retained_views);
                    for(auto &view:_views) {view.epoch=_epoch;view.graph_revision=_graph_revision;}
                    const auto history=_graph_context._states.constFind(_compat_history_node);
                    if(history!=_graph_context._states.cend()) {
                        std::lock_guard history_lock(_history_mutex);
                        if(_history) { _history->clear();for(const auto &row:history->history) _history->push(row); }
                    }
                }
            }
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
    core::perf::Scope trace(core::perf::Stage::WledPublish);
    std::optional<WledFrame> next(WledFrame{mel, hz, magnitude, _network_generation.load(), revision, core::perf::now()});
    // Only the sink format changes: graph Mel frames retain their configured bands.
    // Align triangular Mel-filter centres on the same [0, Nyquist] Mel axis;
    // extend the outermost centre at the edges instead of shifting the range.
    if(next->mel.size()!=50) {
        std::vector<float> formatted(50,0);
        if(!mel.empty()) for(size_t i=0;i<formatted.size();++i) {
            const float centre=std::clamp(float(i+1)*float(mel.size()+1)/51-1,0.f,float(mel.size()-1));
            const auto lo=size_t(centre), hi=std::min(lo+1,mel.size()-1);
            const float fraction=centre-lo;
            formatted[i]=mel[lo]*(1-fraction)+mel[hi]*fraction;
        }
        next->mel.swap(formatted);
    }
    {
        std::lock_guard lock(_wled_mailbox_mutex);
        next.swap(_pending_wled);
    }
    // Retire the superseded frame outside the mailbox lock. DSP never posts a
    // Qt event: GUI/native window operations can contend with Qt event posting.
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
        if (!_wled_timer) {
            _wled_timer = new QTimer(_runtime);
            _wled_timer->setInterval(16);
            _wled_timer->setTimerType(Qt::PreciseTimer);
            QObject::connect(_wled_timer, &QTimer::timeout, _runtime, [this] {
                std::optional<WledFrame> frame;
                {
                    std::lock_guard lock(_wled_mailbox_mutex);
                    frame.swap(_pending_wled);
                }
                if (!frame) return;
                core::perf::duration(core::perf::Stage::WledQueue, core::perf::now() - frame->enqueued_ns);
                if (_wled && frame->generation == _network_generation.load() && frame->graph_revision == graph_revision())
                    _wled->update(frame->mel, frame->hz, frame->magnitude);
            });
        }
        _wled_active = true;
        _wled_timer->start();
        status("WLED", State::Running);
    });
}
void AudioPipeline::stop_wled() {
    command([this] {
        _wled_active = false;
        ++_network_generation;
        if (_wled_timer) _wled_timer->stop();
        std::optional<WledFrame> pending;
        {
            std::lock_guard lock(_wled_mailbox_mutex);
            pending.swap(_pending_wled);
        }
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
    // The owner thread polls this flag. No Qt event posting on the DSP path.
    _history_pending = true;
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
AudioSourceDiagnostics AudioPipeline::capture_diagnostics() const {
    AudioSourceDiagnostics result;
    command([&] { if (_capture) result = _capture->diagnostics(); });
    return result;
}
WLEDAudioReactiveServer *AudioPipeline::wled() const {
    WLEDAudioReactiveServer *result;
    command([&] { result = _wled.get(); });
    return result;
}
} // namespace pipeline
namespace pipeline {
void AudioPipeline::invalidate_views() {
    _views.clear();
    for(const auto &n:_graph->document().value("nodes").toArray()) {
        const auto obj=n.toObject();const auto definition=node_definition(obj.value("internal-data").toObject().value("model-name").toString());
        if(definition.category=="views") {
            TypedFrame empty;empty.available=false;empty.epoch=_epoch;empty.graph_revision=_graph_revision;
            if(definition.input=="history") empty.type=FrameType::History;
            else if(definition.input=="mel") empty.type=FrameType::Mel;
            else if(definition.input=="stereo-spectrum") empty.type=FrameType::StereoSpectrum;
            else if(definition.input=="window") empty.type=FrameType::Window;
            else if(definition.input=="windowed") empty.type=FrameType::Windowed;
            _views[obj.value("id").toInt()]=empty;
        }
    }
    std::function<void(ViewFrame)> handler;
    { std::lock_guard lock(_callback_mutex);handler=_spectrum_handler; }
    if(handler) { ViewFrame view;view.epoch=_epoch;view.graph_revision=_graph_revision;view.views=_views;view.previews=_graph->disabled_output().previews;view.spectrogram=false;handler(std::move(view)); }
}
void AudioPipeline::deliver_typed(const GraphOutput &output) {
    for(auto it=output.views.begin();it!=output.views.end();++it) {
        auto f=it.value();f.epoch=_epoch;f.graph_revision=_graph_revision;_views[it.key()]=std::move(f);
    }
    std::lock_guard callbacks(_typed_callback_mutex);
    for(auto it=output.ports.begin();it!=output.ports.end();++it) {
        if(std::any_of(output.output_events.begin(),output.output_events.end(),[&](const auto &e){return e.first==it.key();})) continue;
        auto f=it.value();f.epoch=_epoch;f.graph_revision=_graph_revision;
        for(const auto &handler:_typed_handlers) if(handler) handler(it.key(),f);
    }
    for(const auto &[id,event]:output.output_events) {
        auto f=event;f.epoch=_epoch;f.graph_revision=_graph_revision;
        for(const auto &handler:_typed_handlers) if(handler) handler(id,f);
    }
}
uint64_t AudioPipeline::subscribe_typed(TypedHandler handler) {
    std::lock_guard lock(_typed_callback_mutex);const auto token=++_next_typed_token;
    _typed_handlers[token]=std::move(handler);return token;
}
void AudioPipeline::unsubscribe_typed(uint64_t token) {
    std::lock_guard lock(_typed_callback_mutex);_typed_handlers.remove(token);
}
void AudioPipeline::set_typed_handler(TypedHandler handler) {
    std::lock_guard lock(_typed_callback_mutex);if(handler) _typed_handlers[0]=std::move(handler);else _typed_handlers.remove(0);
}
QMap<int,TypedFrame> AudioPipeline::latest_views() const {
    std::lock_guard lock(_evaluation_mutex);return _views;
}
uint64_t AudioPipeline::epoch() const { std::lock_guard lock(_evaluation_mutex); return _epoch; }
uint64_t AudioPipeline::ingress_serial(int ingress) const {std::lock_guard lock(_evaluation_mutex);return _ingress_serials.value(ingress);}
void AudioPipeline::clear_ingress(int ingress, uint64_t expected_epoch) {
    std::lock_guard evaluation_lock(_evaluation_mutex);
    if(expected_epoch!=UINT64_MAX && expected_epoch!=_epoch) return;
    const auto ids=_graph->ingress_nodes();if(std::find(ids.begin(),ids.end(),ingress)==ids.end()) return;
    const auto serial=++_ingress_serials[ingress];
    {
        std::lock_guard lock(_mutex);
        if(ingress==_primary_source_id && _analyzer) _analyzer->set_ingress_serial(serial);
        for(auto &branch:_local_branches) if(branch->id==ingress) branch->analyzer->set_ingress_serial(serial);
    }
    const auto affected=_graph->clear_ingress(ingress,_graph_context);
    GraphOutput markers;
    auto frame_type=[](const QString &type) {
        return type=="window"?FrameType::Window:type=="windowed"?FrameType::Windowed:
            type=="stereo-spectrum"?FrameType::StereoSpectrum:type=="spectrum"?FrameType::Spectrum:
            type=="mel"?FrameType::Mel:type=="history"?FrameType::History:FrameType::PCM;
    };
    for(const auto &j:_graph->document().value("nodes").toArray()) {
        const auto node=j.toObject();const int id=node.value("id").toInt();
        if(std::find(affected.begin(),affected.end(),id)==affected.end()) continue;
        const auto parameters=node.value("internal-data").toObject();const auto kind=parameters.value("model-name").toString();
        const auto definition=node_definition(kind);
        TypedFrame marker;marker.available=false;marker.discontinuity=true;marker.epoch=_epoch;marker.graph_revision=_graph_revision;
        marker.type=frame_type(definition.category=="views" || kind=="WLED"?definition.input:_graph->port_type(id));
        marker.stream_id=parameters.value("stream_id").toString().toStdString();
        if(definition.category=="views") markers.views[id]=marker;
        if(definition.category=="outputs") { markers.ports[id]=marker;markers.output_events.emplace_back(id,marker); }
    }
    if(std::find(affected.begin(),affected.end(),_compat_history_node)!=affected.end()) {
        std::lock_guard history_lock(_history_mutex);if(_history) _history->clear();_history_snapshot.store(nullptr);
    }
    deliver_typed(markers);
    if(_wled_active.load() && std::find(affected.begin(),affected.end(),_compat_wled_node)!=affected.end()) {
        ++_network_generation;
        { std::lock_guard lock(_wled_mailbox_mutex);_pending_wled.reset(); }
        publish_wled({},0,0,_graph_revision);
    }
    std::function<void(ViewFrame)> handler;
    {std::lock_guard lock(_callback_mutex);handler=_spectrum_handler;}
    if(handler) {ViewFrame view;view.views=_views;view.epoch=_epoch;view.graph_revision=_graph_revision;view.spectrogram=false;handler(std::move(view));}
}
void AudioPipeline::feed_typed(int ingress, TypedFrame frame, uint64_t expected_epoch, uint64_t expected_ingress_serial) {
    std::lock_guard evaluation_lock(_evaluation_mutex);
    if(!_typed_running || (expected_epoch!=UINT64_MAX && expected_epoch!=_epoch) ||
        (expected_ingress_serial!=UINT64_MAX && expected_ingress_serial!=_ingress_serials.value(ingress))) return;
    if(!frame.available) {
        if(_graph->marker_current(ingress,frame,_graph_context)) clear_ingress(ingress,expected_epoch);
        return;
    }
    if(!frame.sample_rate) return;
    if((frame.type==FrameType::PCM || frame.type==FrameType::Window || frame.type==FrameType::Windowed || frame.type==FrameType::StereoSpectrum) &&
        (frame.left.empty() || frame.left.size()!=frame.right.size() || frame.left.size()>4096)) return;
    if(frame.values.size()>4096 || frame.history.size()>2000) return;
    for(const auto &row:frame.history) if(row.size()>50) return;
    auto ids=_graph->ingress_nodes();if(std::find(ids.begin(),ids.end(),ingress)==ids.end()) return;
    frame.producer_epoch=frame.epoch;
    auto output=_graph->evaluate(ingress,frame,_graph_context,(_view_subscription.load()&8)!=0);
    if(output.ports.isEmpty() && output.views.isEmpty() && output.output_events.empty()) return;
    deliver_typed(output);
    {
        std::lock_guard lock(_history_mutex);
        _processed+=_graph->contains("Window")?output.windows_processed:1;
        if(_history && std::any_of(output.history_events.begin(),output.history_events.end(),[&](const auto &e){return e.first==_compat_history_node;})) {
            if(output.ports.value(_compat_history_node).discontinuity) _history->clear();
            _history->set_history_size(int(output.ports.value(_compat_history_node).history_capacity));
            for(const auto &[id,row]:output.history_events) if(id==_compat_history_node) _history->push(row);
        }
    }
    if(_wled_active.load()) {
        const auto light=output.ports.constFind(_compat_wled_node);
        if(light!=output.ports.cend() && !light->values.empty()) publish_wled(light->values,light->peak_hz,light->magnitude,_graph_revision);
    }
    std::function<void(ViewFrame)> handler;
    { std::lock_guard lock(_callback_mutex);handler=_spectrum_handler; }
    if(handler) {
        ViewFrame view;view.graph_revision=_graph_revision;view.epoch=_epoch;view.ready_timestamp_ns=core::perf::now();
        view.fft=std::move(output.fft);view.mel=std::move(output.mel);view.previews=std::move(output.previews);
        view.spectrogram=output.spectrogram;view.views=_views;
        view.spectrum.sequence=frame.sequence;view.spectrum.generation=frame.generation;view.spectrum.timestamp_ns=frame.timestamp_ns;
        view.spectrum.sample_rate=frame.sample_rate;view.frequency_step=frame.frequency_step;
        handler(std::move(view));
    }
}
}
