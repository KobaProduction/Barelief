#include "pipeline/graph.h"
#include "pipeline/test_signal.h"
#include <desktop/graph_document.h>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
namespace pipeline {
const std::vector<NodeDefinition> &node_definitions() {
    static const std::vector<NodeDefinition> definitions = [] {
    std::vector<NodeDefinition> definitions{
        {"AudioSource", "", "audio", "inputs", {{"source",0}}, {{"source_min",0},{"source_max",1}}},
        {"StereoFFT", "windowed", "stereo-spectrum", "transforms"},
        {"SpectrumMerge", "stereo-spectrum", "spectrum", "transforms", {{"merge",2}}, {{"merge_min",0},{"merge_max",2}}},
        {"SpectrumGain", "spectrum", "spectrum", "transforms", {{"gain",1}}, {{"gain_min",0},{"gain_max",100}}},
        {"Mel", "spectrum", "mel", "transforms", {{"bands",50}}, {{"bands_min",1},{"bands_max",50}}},
        {"Spectrogram", "mel", "history", "transforms", {{"history_size",400}}, {{"history_size_min",2},{"history_size_max",2000}}},
        {"FFTView", "stereo-spectrum", "", "views", {{"channel",0},{"merge",2}}},
        {"MelView", "mel", "", "views"}, {"SpectrogramView", "history", "", "views"},
        {"AudioView", "audio", "", "views"}, {"WLED", "mel", "", "outputs", {{"gain",1}}, {{"gain_min",0},{"gain_max",100}}},
        {"TestSignal", "", "audio", "inputs", TestSignalSettings{}.json()},
        {"Window", "audio", "window", "transforms", {{"size",4096}}, {{"size_min",512},{"size_max",4096}}},
        {"WindowFunction", "window", "windowed", "transforms", {{"function","hann"}}},
        {"BarkWeighting", "stereo-spectrum", "stereo-spectrum", "transforms"},
        {"WindowView", "window", "", "views"}, {"WindowedView", "windowed", "", "views"},
        {"NetworkInput", "", "typed", "inputs", {{"data_type","mel"}}},
        {"NetworkOutput", "typed", "", "outputs", {{"data_type","mel"}}}};
    for(auto &d:definitions) {
        d.defaults["enabled"]=true;
        QJsonObject constraints;
        for(auto it=d.defaults.begin();it!=d.defaults.end();++it) {
            const auto key=it.key(); const auto v=it.value();
            QJsonObject metadata{{"type",v.isBool()?"bool":v.isString()?"string":v.isDouble()?(key=="gain"?"number":"integer"):"json"},{"label",key}};
            if(d.constraints.contains(key+"_min")) metadata["min"]=d.constraints[key+"_min"];
            if(d.constraints.contains(key+"_max")) metadata["max"]=d.constraints[key+"_max"];
            constraints[key]=metadata;
        }
        auto enumeration=[&](QString key,QJsonArray options) { constraints[key]=QJsonObject{{"type","enum"},{"label",key},{"options",options}}; };
        if(d.name=="AudioSource") enumeration("source",{QJsonObject{{"label","System output"},{"value",0}},QJsonObject{{"label","System microphone"},{"value",1}}});
        constraints["enabled"]=QJsonObject{{"type","bool"},{"label","Output enabled"}};
        if(d.name=="WindowFunction") enumeration("function",{QJsonObject{{"label","Hann"},{"value","hann"}},QJsonObject{{"label","Hamming"},{"value","hamming"}},QJsonObject{{"label","Blackman"},{"value","blackman"}},QJsonObject{{"label","Rectangular"},{"value","rectangular"}}});
        if(d.name=="SpectrumMerge" || d.name=="FFTView") enumeration("merge",{{QJsonObject{{"label","Mean"},{"value",0}}}, {QJsonObject{{"label","Maximum"},{"value",1}}}, {QJsonObject{{"label","RMS"},{"value",2}}}});
        if(d.name=="FFTView") enumeration("channel",{{QJsonObject{{"label","Left"},{"value",0}}}, {QJsonObject{{"label","Right"},{"value",1}}}, {QJsonObject{{"label","Merged"},{"value",2}}}});
        if(d.name=="NetworkInput" || d.name=="NetworkOutput") {
            QJsonArray options;for(const auto &t:QStringList{"audio","window","windowed","stereo-spectrum","spectrum","mel","history"}) options.append(QJsonObject{{"label",t},{"value",t}});
            enumeration("data_type",options);
            for(const auto &key:QStringList{"provider_uuid","stream_id","host"}) {
                d.defaults[key]="";constraints[key]=QJsonObject{{"type","string"},{"label",key}};
            }
            d.defaults["port"]=8214;constraints["port"]=QJsonObject{{"type","integer"},{"label","port"},{"min",1},{"max",65535}};
        }
        if(d.name=="TestSignal") {
            auto range=[&](QString key,QString type,double lo,double hi) { constraints[key]=QJsonObject{{"type",type},{"label",key},{"min",lo},{"max",hi}}; };
            range("tones","integer",5,10);range("sample_rate","integer",8000,192000);
            range("minimum_hz","number",10,96000);range("maximum_hz","number",10,96000);
            range("amplitude","number",0,1);range("period_seconds","number",0.1,3600);range("sweep_depth","number",0,0.95);
        }
        d.constraints=constraints;
    }
    return definitions;
    }();
    return definitions;
}
// Legacy version-1 node name, accepted at document boundaries only.
QString canonical_node_name(const QString &name) { return name=="SurfaceView" ? QStringLiteral("SpectrogramView") : name; }
QJsonObject canonical_graph_document(QJsonObject document) {
    if (!document.value("nodes").isArray()) return document;
    auto nodes=document.value("nodes").toArray();
    for (int i=0;i<nodes.size();++i) {
        auto node=nodes[i].toObject(); auto parameters=node.value("internal-data").toObject();
        const auto name=parameters.value("model-name").toString();
        if (name!=canonical_node_name(name)) { parameters["model-name"]=canonical_node_name(name); node["internal-data"]=parameters; nodes[i]=node; }
    }
    if(document.value("version").toInt()==1) {
        QJsonArray edges=document.value("connections").toArray();int next=0;
        QMap<int,QString> kinds;
        for(const auto &v:nodes) {auto n=v.toObject();next=std::max(next,n.value("id").toInt()+1);kinds[n.value("id").toInt()]=n.value("internal-data").toObject().value("model-name").toString();}
        auto add=[&](QString kind,const QJsonObject &near) {
            const int id=next++; auto position=near.value("position").toObject();
            nodes.append(QJsonObject{{"id",id},{"position",position},{"internal-data",QJsonObject{{"model-name",kind}}}});return id;
        };
        auto edge=[](int from,int to) {return QJsonObject{{"outNodeId",from},{"outPortIndex",0},{"inNodeId",to},{"inPortIndex",0}};};
        const auto old_edges=edges;
        for(const auto &v:old_edges) {
            const auto e=v.toObject();const int from=e.value("outNodeId").toInt(),to=e.value("inNodeId").toInt();
            if(kinds[to]!="StereoFFT" || (kinds[from]!="AudioSource" && kinds[from]!="TestSignal")) continue;
            QJsonObject near;for(const auto &n:nodes) if(n.toObject().value("id").toInt()==to) near=n.toObject();
            const int w=add("Window",near),h=add("WindowFunction",near);
            for(int i=0;i<edges.size();++i) if(edges[i]==v) {auto migrated=e;migrated["inNodeId"]=w;edges[i]=migrated;break;}
            edges.append(edge(w,h));edges.append(edge(h,to));
        }
        for(auto it=kinds.begin();it!=kinds.end();++it) if(it.value()=="StereoFFT") {
            const int bark=add("BarkWeighting",{});
            for(int i=0;i<edges.size();++i) { auto e=edges[i].toObject();if(e.value("outNodeId").toInt()==it.key()) {e["outNodeId"]=bark;edges[i]=e;} }
            edges.append(edge(it.key(),bark));
        }
        document["connections"]=edges;document["version"]=2;
    }
    document["nodes"]=nodes; return document;
}
NodeDefinition node_definition(const QString &name) {
    for (const auto &definition : node_definitions())
        if (definition.name == canonical_node_name(name))
            return definition;
    return {};
}
QJsonObject GraphProgram::defaults(int channel, SpectrumMerge merge, int source) {
    QJsonArray nodes, edges;
    auto node = [&](int id, QString kind, double x, double y, QJsonObject params = {}) {
        params["model-name"] = kind;
        nodes.append(QJsonObject{{"id", id}, {"internal-data", params}, {"position", QJsonObject{{"x", x}, {"y", y}}}});
    };
    node(0, "AudioSource", 0, 0, {{"source", source}});
    node(1, "StereoFFT", 250, 0);
    node(2, "SpectrumMerge", 500, 0, {{"merge", static_cast<int>(merge)}});
    node(3, "Mel", 750, 0);
    node(4, "Spectrogram", 1000, 0, {{"history_size", 400}});
    node(5, "FFTView", 500, 250, {{"channel", channel}, {"merge", static_cast<int>(merge)}});
    node(6, "MelView", 1000, 250);
    node(7, "SpectrogramView", 1250, 0);
    node(8, "WLED", 1000, 450, {{"gain", 1.0}});
    node(9, "AudioView", 250, 250);
    for (auto pair : {std::pair{0, 1}, {1, 2}, {2, 3}, {3, 4}, {1, 5}, {3, 6}, {4, 7}, {3, 8}, {0, 9}})
        edges.append(
            QJsonObject{{"outNodeId", pair.first}, {"outPortIndex", 0}, {"inNodeId", pair.second}, {"inPortIndex", 0}});
    return canonical_graph_document({{"version", 1}, {"layout_pending", true}, {"nodes", nodes}, {"connections", edges}});
}
std::shared_ptr<const GraphProgram> GraphProgram::compile(const QJsonObject &original, QString &error) {
    error.clear();
    const auto canonical = canonical_graph_document(original);
    if (canonical.value("version").toInt(-1) != 2) {
        error = "Unsupported pipeline version";
        return {};
    }
    const auto mapped = desktop::read_graph_document(canonical);
    if (!mapped.error.isEmpty()) {
        error = mapped.error;
        return {};
    }
    const auto validation = domain::validate_graph(mapped.graph, domain::desktop_capabilities());
    if (!validation) {
        error = validation.error == domain::GraphError::OutputLimit
                    ? QStringLiteral("Only one WLED output is supported")
                : validation.error == domain::GraphError::Cycle
                    ? QStringLiteral("Pipeline contains a cycle")
                    : QStringLiteral("Invalid graph (%1), node %2").arg(int(validation.error)).arg(validation.node);
        return {};
    }
    auto p = std::make_shared<GraphProgram>();
    p->_document = mapped.document;
    p->_definition = mapped.graph;
    for (const auto id : validation.topological_order) {
        const auto def = std::find_if(mapped.graph.nodes.begin(), mapped.graph.nodes.end(),
                                      [&](const auto &n) { return n.id == id; });
        Node n;
        n.id = id;
        n.kind = QString::fromUtf8(domain::kind_id(def->kind));
        n.enabled = def->enabled;
        for (const auto &json : p->_document["nodes"].toArray())
            if (json.toObject()["id"].toInt() == id)
                n.parameters = json.toObject()["internal-data"].toObject();
        for (const auto &e : mapped.graph.edges)
            if (e.to == id)
                n.input = e.from;
        // Presentation availability until a real/typed-zero tick arrives. The
        // portable executor separately retains a detached node's known clock.
        n.active = n.enabled && (!domain::input_type(*def) || n.input >= 0);
        n.source = def->kind == domain::NodeKind::TestSignal ? 2 : n.parameters.value("source").toInt(0);
        n.channel = n.parameters.value("channel").toInt(0);
        n.history_size = n.parameters.value("history_size").toInt(400);
        n.merge = static_cast<SpectrumMerge>(n.parameters.value("merge").toInt(2));
        n.gain = float(n.parameters.value("gain").toDouble(1));
        if (n.input != -1)
            for (const auto &parent : p->_ordered)
                if (parent.id == n.input)
                    n.active = n.active && parent.active;
        p->_ordered.push_back(std::move(n));
    }
    return p;
}
QJsonObject GraphProgram::test_signal_defaults() {
    auto document = defaults();
    auto nodes = document["nodes"].toArray();
    auto node = nodes[0].toObject();
    auto parameters = TestSignalSettings{}.json();
    parameters["model-name"] = "TestSignal";
    node["internal-data"] = parameters;
    nodes[0] = node;
    document["nodes"] = nodes;
    return document;
}
int GraphProgram::capture_source() const {
    for (const auto &node : _ordered)
        if (node.active && (node.kind == "AudioSource" || node.kind == "TestSignal"))
            return node.source;
    return -1;
}
QJsonObject GraphProgram::source_parameters() const {
    for (const auto &node : _ordered)
        if (node.active && (node.kind == "TestSignal" || node.kind == "AudioSource")) {
            return node.parameters;
        }
    return {};
}
bool GraphProgram::contains(const QString &kind) const {
    return std::any_of(_ordered.begin(), _ordered.end(), [&](const auto &node) { return node.kind == kind; });
}
bool GraphProgram::same_processing_as(const GraphProgram &other) const {
    if (graph_id() != other.graph_id())
        return false;
    if (_ordered.size() != other._ordered.size())
        return false;
    for (size_t i = 0; i < _ordered.size(); ++i) {
        const auto &a = _ordered[i], &b = other._ordered[i];
        if (a.id != b.id || a.input != b.input || a.kind != b.kind)
            return false;
        const auto effective_a = std::find_if(_definition.nodes.begin(), _definition.nodes.end(),
                                              [&](const auto &node) { return node.id == a.id; });
        const auto effective_b = std::find_if(other._definition.nodes.begin(), other._definition.nodes.end(),
                                              [&](const auto &node) { return node.id == b.id; });
        if (effective_a == _definition.nodes.end() || effective_b == other._definition.nodes.end() ||
            *effective_a != *effective_b)
            return false;
    }
    return true;
}

GraphEvaluationContext::GraphEvaluationContext(const GraphEvaluationContext &c)
    : _plan(c._plan), _runtime(c._runtime ? std::make_unique<dataflow_runtime::RuntimeContext>(*c._runtime) : nullptr),
      _ingress(c._ingress), _limits(c._limits), _legacy_sequence(c._legacy_sequence), _program(c._program) {}
GraphEvaluationContext &GraphEvaluationContext::operator=(const GraphEvaluationContext &c) {
    if (this != &c) {
        GraphEvaluationContext copy(c);
        *this = std::move(copy);
    }
    return *this;
}
std::vector<int> GraphProgram::ingress_nodes() const {
    std::vector<int> result;
    for (const auto &n : _ordered)
        if (node_definition(n.kind).input.isEmpty())
            result.push_back(n.id);
    return result;
}
QString GraphProgram::port_type(int id) const {
    for (const auto &n : _ordered)
        if (n.id == id) {
            if (n.kind == "NetworkInput" || n.kind == "NetworkOutput")
                return n.parameters.value("data_type").toString(n.parameters.value("type").toString("mel"));
            return node_definition(n.kind).output;
        }
    return {};
}
signal_contracts::FrameView GraphProgram::frame_view(const TypedFrame &f, std::span<const float> storage) {
    signal_contracts::FrameView v;
    v.type = f.type;
    v.metadata = {f.sequence,     f.generation,  f.epoch,     f.graph_revision, f.source_sequence,  f.producer_epoch,
                  f.timestamp_ns, f.sample_rate, f.fft_size,  f.bands,          f.history_capacity, f.frequency_step,
                  f.peak_hz,      f.magnitude,   f.stream_id, f.discontinuity,  f.bark_weighted,    f.available};
    if (int(f.type) <= 3) {
        v.left = f.left;
        v.right = f.right;
        if (f.type == FrameType::Window && !v.metadata.fft_size)
            v.metadata.fft_size = v.left.size();
    } else if (f.type != FrameType::History)
        v.values = f.values;
    else if (!f.history.empty())
        v.history = {storage, f.history.front().size(), f.history.size(), f.history.size(), 0};
    return v;
}
TypedFrame GraphProgram::owned_snapshot(const signal_contracts::FrameView &v) {
    TypedFrame f;
    f.type = v.type;
    const auto &m = v.metadata;
    f.sequence = m.sequence;
    f.generation = m.generation;
    f.epoch = m.epoch;
    f.graph_revision = m.graph_revision;
    f.source_sequence = m.source_sequence;
    f.producer_epoch = m.producer_epoch;
    f.timestamp_ns = m.timestamp_ns;
    f.sample_rate = m.sample_rate;
    f.fft_size = m.fft_size;
    f.bands = m.bands;
    f.history_capacity = m.history_capacity;
    f.frequency_step = m.frequency_step;
    f.peak_hz = m.peak_hz;
    f.magnitude = m.magnitude;
    f.stream_id = m.stream_id;
    f.discontinuity = m.discontinuity;
    f.bark_weighted = m.bark_weighted;
    f.available = m.available;
    f.left.assign(v.left.begin(), v.left.end());
    f.right.assign(v.right.begin(), v.right.end());
    f.values.assign(v.values.begin(), v.values.end());
    for (size_t row = 0; row < v.history.frames; ++row) {
        const auto r = v.history.row(row);
        f.history.emplace_back(r.begin(), r.end());
    }
    if (f.type == FrameType::History && f.values.empty() && !f.history.empty())
        f.values = f.history.back();
    return f;
}
signal_contracts::Status GraphProgram::prepare_ingress(int id, const signal_contracts::FrameView &f,
                                                       GraphEvaluationContext &c) const {
    using signal_contracts::Status;
    const auto validated = signal_contracts::validate(f);
    if (validated != Status::ok)
        return validated;
    const bool same_program = c._program == this;
    const auto actual = signal_contracts::contract(f);
    auto specs = same_program ? c._ingress : std::vector<dataflow_runtime::IngressSpec>{};
    auto it = std::find_if(specs.begin(), specs.end(), [&](const auto &s) { return s.node == id; });
    auto compare = actual;
    if (it != specs.end() && actual.type == FrameType::PCM && actual.columns <= it->max_samples)
        compare.columns = it->max_samples;
    if (same_program && c._plan && it != specs.end() && compare == it->contract)
        return Status::ok;
    dataflow_runtime::IngressSpec spec{id, actual, actual.columns};
    spec.allow_legacy_window = actual.type == FrameType::Window;
    if (actual.type == FrameType::PCM)
        spec.contract.columns = spec.max_samples = std::max(std::size_t{4096}, actual.columns);
    if (it == specs.end())
        specs.push_back(spec);
    else
        *it = spec;
    std::shared_ptr<const dataflow_runtime::PreparedGraph> plan;
    std::unique_ptr<dataflow_runtime::RuntimeContext> context;
    auto limits = c._limits;
    for (const auto &ingress : specs)
        limits.max_frame_samples = std::max(limits.max_frame_samples, ingress.max_samples);
    const auto status = dataflow_runtime::PreparedGraph::prepare(_definition, specs, limits, plan, context,
                                                                 same_program ? c._plan.get() : nullptr,
                                                                 same_program ? c._runtime.get() : nullptr);
    if (status != Status::ok)
        return status;
    c._plan = std::move(plan);
    c._runtime = std::move(context);
    c._ingress = std::move(specs);
    c._limits = limits;
    if (!same_program)
        c._legacy_sequence.clear();
    c._program = this;
    return Status::ok;
}
signal_contracts::Status GraphProgram::process_view(int id, const signal_contracts::FrameView &f,
                                                    GraphEvaluationContext &c,
                                                    dataflow_runtime::OutputCallback callback,
                                                    void *user) const noexcept {
    if (c._program != this || !c._plan || !c._runtime)
        return signal_contracts::Status::unavailable;
    return c._plan->process(id, f, *c._runtime, callback, user);
}
std::vector<int> GraphProgram::clear_ingress(int id, GraphEvaluationContext &c) const {
    if (c._program != this)
        c = GraphEvaluationContext{};
    if (c._plan)
        return c._plan->clear_ingress(id, *c._runtime);
    std::vector<int> affected;
    for (const auto &n : _ordered)
        if (n.id == id || std::find(affected.begin(), affected.end(), n.input) != affected.end())
            affected.push_back(n.id);
    return affected;
}
bool GraphProgram::marker_current(int id, const TypedFrame &f, const GraphEvaluationContext &c) const {
    return !c._plan || c._plan->marker_current(id, frame_view(f), *c._runtime);
}
GraphOutput GraphProgram::disabled_output(bool previews) const {
    GraphOutput result;
    for (const auto &node : _ordered)
        if (!node.active) {
            const auto definition = node_definition(node.kind);
            const auto type =
                definition.category == "views" || node.kind == "WLED" ? definition.input : port_type(node.id);
            TypedFrame marker;
            marker.available = false;
            marker.discontinuity = true;
            marker.type = type == "window"            ? FrameType::Window
                          : type == "windowed"        ? FrameType::Windowed
                          : type == "stereo-spectrum" ? FrameType::StereoSpectrum
                          : type == "spectrum"        ? FrameType::Spectrum
                          : type == "mel"             ? FrameType::Mel
                          : type == "history"         ? FrameType::History
                                                      : FrameType::PCM;
            marker.stream_id = node.parameters.value("stream_id").toString().toStdString();
            if (definition.category == "views")
                result.views[node.id] = marker;
            else
                result.ports[node.id] = marker;
            if (definition.category == "outputs")
                result.output_events.emplace_back(node.id, marker);
            if (previews)
                result.previews[node.id] = {};
        }
    return result;
}
std::vector<int> GraphProgram::rebind(GraphEvaluationContext &c, const GraphProgram &previous, uint64_t from,
                                      uint64_t to, signal_contracts::Status *status_out) const {
    if (status_out)
        *status_out = signal_contracts::Status::ok;
    if (!c._plan) {
        c._program = this;
        return {};
    }
    std::shared_ptr<const dataflow_runtime::PreparedGraph> p;
    std::unique_ptr<dataflow_runtime::RuntimeContext> context;
    auto specs = c._ingress;
    const auto ids = ingress_nodes();
    std::erase_if(specs, [&](const auto &spec) {
        const auto node = std::find_if(_definition.nodes.begin(), _definition.nodes.end(),
                                       [&](const auto &n) { return n.id == spec.node; });
        return std::find(ids.begin(), ids.end(), spec.node) == ids.end() ||
               (node != _definition.nodes.end() && node->kind == domain::NodeKind::NetworkInput &&
                domain::output_type(*node) != spec.contract.type);
    });
    const auto status = dataflow_runtime::PreparedGraph::prepare(_definition, specs, c._limits, p, context,
                                                                 c._plan.get(), c._runtime.get());
    if (status_out)
        *status_out = status;
    if (status != signal_contracts::Status::ok) {
        return {};
    }
    const auto resets = p->reset_nodes();
    p->rebase_local_producers(*context, from, to, 0);
    c._plan = std::move(p);
    c._runtime = std::move(context);
    c._ingress = std::move(specs);
    c._program = this;
    return resets;
}
std::vector<std::vector<float>> GraphProgram::history_snapshot(int id, const GraphEvaluationContext &c) const {
    std::vector<std::vector<float>> rows;
    if (!c._plan)
        return rows;
    const auto h = c._plan->history(id, *c._runtime);
    rows.reserve(h.frames);
    for (size_t i = 0; i < h.frames; ++i) {
        const auto row = h.row(i);
        rows.emplace_back(row.begin(), row.end());
    }
    return rows;
}
int GraphProgram::history_ingress(int id, const GraphEvaluationContext &c) const noexcept {
    return c._plan ? c._plan->history_ingress(id) : -1;
}
void GraphProgram::reset_history(int id, GraphEvaluationContext &c) const noexcept {
    if (c._plan)
        c._plan->reset_history(id, *c._runtime);
}
void GraphProgram::rebase_local_producers(GraphEvaluationContext &c, uint64_t from, uint64_t to,
                                          uint64_t generation) const noexcept {
    if (c._plan)
        c._plan->rebase_local_producers(*c._runtime, from, to, generation);
}
GraphOutput GraphProgram::evaluate(const SpectrumFrame &f, bool previews) const {
    GraphEvaluationContext c;
    return evaluate(f, c, previews);
}
GraphOutput GraphProgram::evaluate(const SpectrumFrame &f, GraphEvaluationContext &c, bool previews) const {
    TypedFrame input;
    input.type = FrameType::StereoSpectrum;
    input.left = f.left;
    input.right = f.right;
    input.sample_rate = f.sample_rate;
    input.frequency_step = f.frequency_step ? f.frequency_step
                           : f.left.empty() ? 0
                                            : float(f.sample_rate) / (2 * f.left.size());
    input.fft_size = f.left.size() * 2;
    input.bark_weighted = true;
    input.timestamp_ns = f.timestamp_ns;
    input.sequence = f.sequence;
    input.generation = f.generation;
    input.discontinuity = f.discontinuity;
    const auto ids = ingress_nodes();
    return ids.empty() ? GraphOutput{} : evaluate(ids.front(), input, c, previews);
}
GraphOutput GraphProgram::evaluate(int id, const TypedFrame &input, GraphEvaluationContext &c, bool previews) const {
    GraphOutput result;
    if (!input.available)
        return result;
    std::vector<float> storage;
    if (input.type == FrameType::History)
        for (const auto &row : input.history)
            storage.insert(storage.end(), row.begin(), row.end());
    auto view = frame_view(input, storage);
    // Old desktop callers have optional sequence/bands fields. Normalize only
    // this compatibility boundary; the portable processor validates its input.
    const bool legacy_sequence = !view.metadata.sequence;
    if (legacy_sequence)
        view.metadata.sequence = c._program == this ? c._legacy_sequence.value(id) + 1 : 1;
    if (view.type == FrameType::Mel)
        view.metadata.bands = view.values.size();
    if (view.type == FrameType::History && !view.metadata.history_capacity)
        view.metadata.history_capacity = view.history.capacity;
    if (prepare_ingress(id, view, c) != signal_contracts::Status::ok)
        return result;
    if (legacy_sequence)
        c._legacy_sequence[id] = view.metadata.sequence;
    struct Sink {
        GraphOutput *out;
        bool previews;
    };
    Sink sink{&result, previews};
    const auto callback = [](void *user, const dataflow_runtime::OutputView &output) {
        auto &sink = *static_cast<Sink *>(user);
        auto &r = *sink.out;
        const auto kind = QString::fromUtf8(domain::kind_id(output.kind));
        auto f = GraphProgram::owned_snapshot(output.frame);
        const auto d = node_definition(kind);
        if (!output.display_values.empty())
            f.values.assign(output.display_values.begin(), output.display_values.end());
        if (output.window_emitted)
            ++r.windows_processed;
        if (d.category == "views")
            r.views[output.node] = f;
        if (!d.output.isEmpty() || d.category == "outputs")
            r.ports[output.node] = f;
        if (d.category == "outputs")
            r.output_events.emplace_back(output.node, f);
        if (kind == "FFTView")
            r.fft = f.values;
        else if (kind == "MelView")
            r.mel = f.values;
        else if (kind == "SpectrogramView")
            r.spectrogram = true;
        else if (kind == "Spectrogram") {
            r.history = f.values;
            r.history_size = int(f.history_capacity);
            r.history_events.emplace_back(output.node, f.values);
        } else if (kind == "WLED") {
            r.network = f.values;
            r.network_node = output.node;
            r.peak_hz = f.peak_hz;
            r.magnitude = f.magnitude;
        }
        if (sink.previews) {
            const auto &values = f.values.empty() ? f.left : f.values;
            float peak = 0;
            for (float x : values)
                peak = std::max(peak, std::abs(x));
            r.previews[output.node] = {values.size(), peak};
        }
    };
    process_view(id, view, c, callback, &sink);
    // Preserve desktop event ordering: node schedule first, then source cadence.
    auto rank = [&](int id) {
        for (size_t i = 0; i < _ordered.size(); ++i)
            if (_ordered[i].id == id)
                return i;
        return _ordered.size();
    };
    std::stable_sort(result.output_events.begin(), result.output_events.end(),
                     [&](const auto &a, const auto &b) { return rank(a.first) < rank(b.first); });
    std::stable_sort(result.history_events.begin(), result.history_events.end(),
                     [&](const auto &a, const auto &b) { return rank(a.first) < rank(b.first); });
    return result;
}
} // namespace pipeline
