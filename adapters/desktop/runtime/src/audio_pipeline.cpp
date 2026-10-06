#include "core/performance.h"
#include "pipeline/audio_pipeline.h"
#include <QMetaObject>
#include <chrono>
#include <algorithm>
#include <exception>
#include <QJsonArray>
#include <QScopedValueRollback>
#include <desktop/graph_document.h>
namespace pipeline {
signal_contracts::Status AudioPipeline::prepare_runtime(const domain::GraphDefinition &graph,
        std::vector<dataflow_runtime::IngressSpec> specs, application::RuntimeLimits limits) {
    _executor.ingress_specs(specs);
    _lifecycle.runtime_limits(limits);
    auto result=_lifecycle.prepare(graph);
    if(result) result=_lifecycle.apply();
    if(!result) {
        _executor.ingress_specs(_ingress_specs);
        _lifecycle.runtime_limits(_runtime_limits);
        return result.status;
    }
    _ingress_specs=std::move(specs);
    _runtime_limits=limits;
    return signal_contracts::Status::ok;
}
signal_contracts::Status AudioPipeline::negotiate_ingress(int id,const signal_contracts::FrameView &frame) {
    using signal_contracts::Status;
    const auto valid=signal_contracts::validate(frame);
    if(valid!=Status::ok) return valid;
    const auto actual=signal_contracts::contract(frame);
    const auto existing=std::find_if(_ingress_specs.begin(),_ingress_specs.end(),[&](const auto &s){return s.node==id;});
    auto compare=actual;
    if(existing!=_ingress_specs.end() && actual.type==FrameType::PCM && actual.columns<=existing->max_samples)
        compare.columns=existing->max_samples;
    if(existing!=_ingress_specs.end() && compare==existing->contract) return Status::ok;
    auto specs=_ingress_specs;
    auto it=std::find_if(specs.begin(),specs.end(),[&](const auto &s){return s.node==id;});
    dataflow_runtime::IngressSpec spec{id,actual,actual.columns};
    spec.allow_legacy_window=actual.type==FrameType::Window;
    if(actual.type==FrameType::PCM) spec.contract.columns=spec.max_samples=std::max(size_t{4096},actual.columns);
    if(it==specs.end()) specs.push_back(spec);else *it=spec;
    // Preserve the desktop negotiated per-ingress capacity policy. Preparation
    // checks aggregate memory and every ingress against this checked ceiling.
    auto limits=_runtime_limits;
    for(const auto &ingress:specs) limits.max_frame_samples=std::max(limits.max_frame_samples,ingress.max_samples);
    return prepare_runtime(_executor.plan()->definition(),std::move(specs),limits);
}
std::vector<std::vector<float>> AudioPipeline::runtime_history(int node) const {
    std::vector<std::vector<float>> result;
    if(!_executor.plan() || !_executor.context()) return result;
    const auto history=_executor.plan()->history(node,*_executor.context());
    result.reserve(history.frames);
    for(size_t i=0;i<history.frames;++i) {
        const auto row=history.row(i);result.emplace_back(row.begin(),row.end());
    }
    return result;
}

AudioPipeline::AudioPipeline() : AudioPipeline(default_audio_source_factory()) {}
AudioPipeline::AudioPipeline(AudioSourceFactory source_factory) : _source_factory(std::move(source_factory)) {
    if (!_source_factory) _source_factory = default_audio_source_factory();
    QString error;
    _graph = GraphProgram::compile(GraphProgram::defaults(), error);
    const auto definition=desktop::read_graph_document(_graph->document());
    if(prepare_runtime(definition.graph,{},_runtime_limits)!=signal_contracts::Status::ok)
        throw std::runtime_error("Default graph preparation failed");
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
        _lifecycle.start();
        const auto generation = ++_analyzer_generation;
        if (_reconfiguring_sources && _executor.plan())
            _executor.plan()->rebase_local_producers(*_executor.context(),_epoch,_epoch,generation);
        _history_snapshot.store(nullptr);
        _history_pending = false;
        _history_requested_ns = 0;
        _analyzer = std::make_unique<Analyzer>();
        _analyzer->set_mel_enabled(false);
        _analyzer->set_pcm_only(true);
        _analyzer->set_stream_epoch(_epoch);
        {
            std::lock_guard history_lock(_history_mutex);
            if (!_reconfiguring_sources || !_history) {
                _history = std::make_unique<Spectrogram>(400);
                _processed = 0;
            }
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
            if (!_reconfiguring_sources) _history.reset();
        }
        _history_snapshot.store(nullptr);
        if (!_reconfiguring_sources) {
            std::lock_guard evaluation_lock(_evaluation_mutex);_typed_running=false;++_epoch;_lifecycle.stop();_legacy_sequence.clear();invalidate_views();
            if(_wled_active.load()) publish_wled({},0,0,_graph_revision);
        }
        status("Analyzer", _reconfiguring_sources ? State::Starting : State::Stopped);
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
        if (!_reconfiguring_sources) {
            std::lock_guard evaluation_lock(_evaluation_mutex);++_epoch;_lifecycle.stop();
            if(_typed_running) _lifecycle.start();
            _legacy_sequence.clear();
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
        TypedFrame input;
        input.type=frame.raw_window?(frame.raw_pcm?FrameType::PCM:FrameType::Window):FrameType::StereoSpectrum;
        input.left=frame.left;input.right=frame.right;
        input.sample_rate=frame.sample_rate;input.sequence=frame.sequence;input.generation=frame.generation;
        input.timestamp_ns=frame.timestamp_ns;input.discontinuity=frame.discontinuity;
        input.epoch=_epoch;input.graph_revision=revision;
        if(!frame.raw_window) {
            input.fft_size=frame.left.size()*2;input.bark_weighted=true;
            input.frequency_step=frame.frequency_step?frame.frequency_step:frame.left.empty()?0:float(frame.sample_rate)/(2*frame.left.size());
        }
        if(ingress_node>=0) return evaluate_view(ingress_node,input,*program);
        auto ids=program->ingress_nodes();
        for(auto id:ids) if(program->port_type(id)=="audio") return evaluate_view(id,input,*program);
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
        const bool bootstrap=!_graph_applied && _graph->capture_source()>=0;
        _graph_applied=true;
        lock.unlock();evaluation_lock.unlock();
        if(bootstrap) command([this] {if(!_explicit_stop) {start_analyzer();start_capture();}});
        return true;
    }
    const auto previous_epoch=_epoch;
    const auto definition=desktop::read_graph_document(program->document());
    auto specs=_ingress_specs;
    // IDs persist, but a replaced source must negotiate its own semantic contract.
    std::erase_if(specs,[&](const auto &spec) {
        const auto node=std::find_if(definition.graph.nodes.begin(),definition.graph.nodes.end(),
                                    [&](const auto &n){return n.id==spec.node;});
        if(node==definition.graph.nodes.end() || domain::input_type(*node)) return true;
        if(node->kind==domain::NodeKind::NetworkInput) return domain::output_type(*node)!=spec.contract.type;
        return spec.contract.type!=FrameType::PCM && spec.contract.type!=FrameType::StereoSpectrum &&
               !(spec.allow_legacy_window && spec.contract.type==FrameType::Window);
    });
    const auto preparation_status=prepare_runtime(definition.graph,std::move(specs),_runtime_limits);
    if(preparation_status!=signal_contracts::Status::ok) {
        error=QStringLiteral("Graph preparation failed (%1)").arg(int(preparation_status));
        return false;
    }
    _graph = std::move(program);
    const auto reset_nodes=_executor.plan()->reset_nodes();
    _executor.plan()->rebase_local_producers(*_executor.context(),previous_epoch,previous_epoch+1,0);
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
    // A disconnected scalar/vector view has no live source until the next
    // typed silence tick. Notify presentation now; compatible history stays.
    for(auto it=blocked.views.begin();it!=blocked.views.end();++it) if(it->type!=FrameType::History) {
        auto marker=it.value();marker.epoch=_epoch;marker.graph_revision=_graph_revision;
        _views[it.key()]=std::move(marker);
    }
    std::function<void(ViewFrame)> view_handler;
    {std::lock_guard callbacks(_callback_mutex);view_handler=_spectrum_handler;}
    if(view_handler) {
        ViewFrame update;update.views=_views;update.epoch=_epoch;update.graph_revision=_graph_revision;
        view_handler(std::move(update));
    }
    if(_wled_active.load() && blocked.ports.contains(_compat_wled_node)) {
        {std::lock_guard mailbox_lock(_wled_mailbox_mutex);_pending_wled.reset();}
        publish_wled({},0,0,_graph_revision);
    }
    _graph_applied=true;
    evaluation_lock.unlock();
    command([this,source_changed] {
            QScopedValueRollback<bool> preserve_runtime(_reconfiguring_sources,source_changed);
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
            // A network-only graph runs the application executor without a
            // physical producer or Analyzer worker. Its running status must
            // admit stream bindings after auto-starting Apply as well.
            if(!local && !stopped) {
                bool processing;
                {std::lock_guard evaluation_lock(_evaluation_mutex);processing=_executor.running();}
                if(processing) status("Analyzer",State::Running);
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
bool AudioPipeline::process_without_delivery(int ingress,const TypedFrame &input) {
    if(_wled_active.load()) return false;
    {std::lock_guard lock(_callback_mutex);if((_view_subscription.load()&1) && _spectrum_handler)return false;}
    {std::lock_guard lock(_typed_callback_mutex);if(!_typed_handlers.isEmpty())return false;}
    // Row-layout conversion reuses its bounded buffer on unchanged geometry.
    if(input.type==FrameType::History) {
        size_t size=0;for(const auto &row:input.history)size+=row.size();
        _history_input_storage.resize(size);
        auto destination=_history_input_storage.begin();
        for(const auto &row:input.history)destination=std::copy(row.begin(),row.end(),destination);
    }
    auto frame=GraphProgram::frame_view(input,_history_input_storage);
    frame.metadata.producer_epoch=input.epoch;
    if(!frame.metadata.sequence)frame.metadata.sequence=++_legacy_sequence[ingress];
    if(frame.type==FrameType::Mel)frame.metadata.bands=frame.values.size();
    if(frame.type==FrameType::History && !frame.metadata.history_capacity)frame.metadata.history_capacity=frame.history.capacity;
    if(negotiate_ingress(ingress,frame)!=signal_contracts::Status::ok)return true;
    struct Sink {AudioPipeline *owner;size_t windows=0;bool emitted=false;};
    Sink sink{this};
    _executor.process(ingress,frame,[](void *user,const dataflow_runtime::OutputView &output) {
        auto &sink=*static_cast<Sink *>(user);auto &owner=*sink.owner;
        sink.emitted=true;sink.windows+=output.window_emitted;
        using K=domain::NodeKind;
        const bool view=output.kind==K::AudioView || output.kind==K::FFTView || output.kind==K::MelView ||
                        output.kind==K::SpectrogramView || output.kind==K::WindowView || output.kind==K::WindowedView;
        if(view) {
            // Reuse bounded owned payload buffers. Never retain runtime spans.
            auto &f=owner._views[output.node];const auto &v=output.frame;const auto &m=v.metadata;
            f.type=v.type;f.sequence=m.sequence;f.generation=m.generation;f.epoch=owner._epoch;
            f.graph_revision=owner._graph_revision;f.source_sequence=m.source_sequence;f.producer_epoch=m.producer_epoch;
            f.timestamp_ns=m.timestamp_ns;f.sample_rate=m.sample_rate;f.fft_size=m.fft_size;f.bands=m.bands;
            f.history_capacity=m.history_capacity;f.frequency_step=m.frequency_step;f.peak_hz=m.peak_hz;f.magnitude=m.magnitude;
            f.stream_id.assign(m.stream_id);f.discontinuity=m.discontinuity;f.bark_weighted=m.bark_weighted;f.available=m.available;
            f.left.assign(v.left.begin(),v.left.end());f.right.assign(v.right.begin(),v.right.end());
            auto values=output.display_values.empty()?v.values:output.display_values;
            if(v.type==FrameType::History && v.history.frames) {
                values=v.history.row(v.history.frames-1);
                owner._history_zero_latest[output.node]=!v.history.newest_override.empty();
            }
            f.values.assign(values.begin(),values.end());f.history.clear();
        }
        if(output.node==owner._compat_history_node && output.kind==K::Spectrogram && output.frame.history.frames) {
            std::lock_guard lock(owner._history_mutex);
            if(owner._history) {
                const auto &v=output.frame;
                if(v.metadata.discontinuity)owner._history->clear();
                owner._history->set_history_size(v.metadata.history_capacity);
                const auto row=v.history.row(v.history.frames-1);
                owner._compat_history_row.assign(row.begin(),row.end());
                owner._history->push(owner._compat_history_row);
            }
        }
    },&sink);
    if(sink.emitted) {
        if(input.type==FrameType::History)_incoming_histories[ingress]=input;
        const auto &nodes=_executor.plan()->definition().nodes;
        const bool window=std::any_of(nodes.begin(),nodes.end(),[](const auto &n){return n.kind==domain::NodeKind::Window;});
        std::lock_guard lock(_history_mutex);_processed+=window?sink.windows:1;
    }
    return true;
}
GraphOutput AudioPipeline::evaluate_view(int ingress,const TypedFrame &input,const GraphProgram &program) {
    GraphOutput result;
    std::vector<float> history_storage;
    if(input.type==FrameType::History)
        for(const auto &row:input.history)history_storage.insert(history_storage.end(),row.begin(),row.end());
    auto frame=GraphProgram::frame_view(input,history_storage);
    frame.metadata.producer_epoch=input.epoch;
    if(!frame.metadata.sequence)frame.metadata.sequence=++_legacy_sequence[ingress];
    if(frame.type==FrameType::Mel)frame.metadata.bands=frame.values.size();
    if(frame.type==FrameType::History && !frame.metadata.history_capacity)frame.metadata.history_capacity=frame.history.capacity;
    if(negotiate_ingress(ingress,frame)!=signal_contracts::Status::ok)return result;
    struct Sink {AudioPipeline *owner;GraphOutput *result;bool previews;};
    Sink sink{this,&result,(_view_subscription.load()&8)!=0};
    _executor.process(ingress,frame,[](void *user,const dataflow_runtime::OutputView &output) {
        auto &sink=*static_cast<Sink *>(user);auto &r=*sink.result;
        auto borrowed=output.frame;
        // Keep the history ring in the runtime. Presentation snapshots separately.
        borrowed.history={};
        auto f=GraphProgram::owned_snapshot(borrowed);
        if(output.frame.type==FrameType::History && output.frame.history.frames) {
            const auto row=output.frame.history.row(output.frame.history.frames-1);
            f.values.assign(row.begin(),row.end());
            sink.owner->_history_zero_latest[output.node]=!output.frame.history.newest_override.empty();
        }
        if(!output.display_values.empty())f.values.assign(output.display_values.begin(),output.display_values.end());
        const auto kind=QString::fromUtf8(domain::kind_id(output.kind));
        const auto definition=node_definition(kind);
        if(output.window_emitted)++r.windows_processed;
        if(definition.category=="views")r.views[output.node]=f;
        if(!definition.output.isEmpty() || definition.category=="outputs")r.ports[output.node]=f;
        if(definition.category=="outputs")r.output_events.emplace_back(output.node,f);
        if(kind=="FFTView")r.fft=f.values;
        else if(kind=="MelView")r.mel=f.values;
        else if(kind=="SpectrogramView")r.spectrogram=true;
        else if(kind=="Spectrogram") {
            r.history_size=int(f.history_capacity);r.history_events.emplace_back(output.node,f.values);
        }else if(kind=="WLED") {
            r.network=f.values;r.network_node=output.node;r.peak_hz=f.peak_hz;r.magnitude=f.magnitude;
        }
        if(sink.previews) {
            const auto &values=f.values.empty()?f.left:f.values;
            float peak=0;for(float value:values)peak=std::max(peak,std::abs(value));
            r.previews[output.node]={values.size(),peak};
        }
    },&sink);
    return result;
}
void AudioPipeline::invalidate_views() {
    _incoming_histories.clear();
    _history_zero_latest.clear();
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
    if(_typed_handlers.isEmpty())return;
    const auto interested=[this](uint64_t token,int node) {
        const auto selected=_typed_nodes.constFind(token);
        return selected==_typed_nodes.cend() || std::find(selected->begin(),selected->end(),node)!=selected->end();
    };
    const auto has_consumer=[&](int node) {
        for(auto it=_typed_handlers.cbegin();it!=_typed_handlers.cend();++it)
            if(it.value() && interested(it.key(),node))return true;
        return false;
    };
    const auto snapshot=[this](int id,TypedFrame f) {
        if(f.available && f.type==FrameType::History && f.history.empty()) {
            const auto ingress=_executor.plan()->history_ingress(id);
            f.history=ingress>=0?_incoming_histories.value(ingress).history:runtime_history(id);
            if(ingress>=0 && _history_zero_latest.value(id) && !f.history.empty())
                std::fill(f.history.back().begin(),f.history.back().end(),0);
        }
        return f;
    };
    for(auto it=output.ports.begin();it!=output.ports.end();++it) {
        if(std::any_of(output.output_events.begin(),output.output_events.end(),[&](const auto &e){return e.first==it.key();})) continue;
        if(!has_consumer(it.key()))continue;
        auto f=snapshot(it.key(),it.value());f.epoch=_epoch;f.graph_revision=_graph_revision;
        for(auto handler=_typed_handlers.cbegin();handler!=_typed_handlers.cend();++handler)
            if(handler.value() && interested(handler.key(),it.key()))handler.value()(it.key(),f);
    }
    for(const auto &[id,event]:output.output_events) {
        if(!has_consumer(id))continue;
        auto f=snapshot(id,event);f.epoch=_epoch;f.graph_revision=_graph_revision;
        for(auto handler=_typed_handlers.cbegin();handler!=_typed_handlers.cend();++handler)
            if(handler.value() && interested(handler.key(),id))handler.value()(id,f);
    }
}
uint64_t AudioPipeline::subscribe_typed(TypedHandler handler) {
    std::lock_guard lock(_typed_callback_mutex);const auto token=++_next_typed_token;
    _typed_handlers[token]=std::move(handler);return token;
}
uint64_t AudioPipeline::subscribe_typed(TypedHandler handler,std::vector<int> nodes) {
    std::lock_guard lock(_typed_callback_mutex);const auto token=++_next_typed_token;
    _typed_handlers[token]=std::move(handler);_typed_nodes[token]=std::move(nodes);return token;
}
void AudioPipeline::unsubscribe_typed(uint64_t token) {
    std::lock_guard lock(_typed_callback_mutex);_typed_handlers.remove(token);_typed_nodes.remove(token);
}
void AudioPipeline::set_typed_handler(TypedHandler handler) {
    std::lock_guard lock(_typed_callback_mutex);if(handler) _typed_handlers[0]=std::move(handler);else _typed_handlers.remove(0);
}
QMap<int,TypedFrame> AudioPipeline::latest_views() const {
    std::lock_guard lock(_evaluation_mutex);
    QMap<int,TypedFrame> views;
    // Snapshot ownership belongs to this requested boundary; sharing the cache's
    // QMap would force detach/container allocation on its next processing tick.
    for(auto it=_views.cbegin();it!=_views.cend();++it)views.insert(it.key(),it.value());
    for(auto it=views.begin();it!=views.end();++it) if(it->available && it->type==FrameType::History) {
        const auto ingress=_executor.plan()->history_ingress(it.key());
        if(ingress>=0) {
            it->history=_incoming_histories.value(ingress).history;
            if(_history_zero_latest.value(it.key()) && !it->history.empty())
                std::fill(it->history.back().begin(),it->history.back().end(),0);
        }
        else it->history=runtime_history(it.key());
    }
    return views;
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
    const auto affected=_executor.plan()->clear_ingress(ingress,*_executor.context());
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
void AudioPipeline::feed_typed(int ingress, const TypedFrame &frame, uint64_t expected_epoch, uint64_t expected_ingress_serial) {
    std::lock_guard evaluation_lock(_evaluation_mutex);
    if(!_typed_running || (expected_epoch!=UINT64_MAX && expected_epoch!=_epoch) ||
        (expected_ingress_serial!=UINT64_MAX && expected_ingress_serial!=_ingress_serials.value(ingress))) return;
    if(!frame.available) {
        if(_executor.plan()->marker_current(ingress,GraphProgram::frame_view(frame),*_executor.context()))
            clear_ingress(ingress,expected_epoch);
        return;
    }
    if(!frame.sample_rate) return;
    if((frame.type==FrameType::PCM || frame.type==FrameType::Window || frame.type==FrameType::Windowed || frame.type==FrameType::StereoSpectrum) &&
        (frame.left.empty() || frame.left.size()!=frame.right.size() || frame.left.size()>4096)) return;
    if(frame.values.size()>4096 || frame.history.size()>2000) return;
    for(const auto &row:frame.history) if(row.size()>50) return;
    const auto &nodes=_executor.plan()->definition().nodes;
    if(std::none_of(nodes.begin(),nodes.end(),[&](const auto &n){return n.id==ingress && !domain::input_type(n);}))return;
    if(process_without_delivery(ingress,frame))return;
    auto output=evaluate_view(ingress,frame,*_graph);
    if(output.ports.isEmpty() && output.views.isEmpty() && output.output_events.empty()) return;
    if(frame.type==FrameType::History)_incoming_histories[ingress]=std::move(frame);
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
