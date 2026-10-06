#include "pipeline/graph.h"
#include "pipeline/test_signal.h"
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
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
    const auto document=canonical_graph_document(original);
    error.clear();
    auto fail = [&](QString message) -> std::shared_ptr<const GraphProgram> {
        error = message;
        return {};
    };
    if (document.value("version").toInt(-1) != 1 && document.value("version").toInt(-1) != 2)
        return fail("Unsupported pipeline version (expected 1 or 2)");
    const auto nodes = document.value("nodes").toArray();
    const auto connections = document.value("connections").toArray();
    if (nodes.isEmpty() || nodes.size() > 128 || connections.size() > 256)
        return fail("Pipeline must contain 1–128 nodes and at most 256 connections");
    QMap<int, Node> by_id;
    int wled_outputs=0;
    for (const auto &value : nodes) {
        const auto json = value.toObject();
        const auto id = json.value("id").toInt(-1);
        auto params = json.value("internal-data").toObject();
        const auto kind = canonical_node_name(params.value("model-name").toString());
        params["model-name"]=kind;
        if (id < 0 || id == std::numeric_limits<int>::max() || by_id.contains(id))
            return fail("Invalid or duplicate node ID");
        if (node_definition(kind).name.isEmpty())
            return fail("Unknown audio node: " + kind);
        if(kind=="WLED" && ++wled_outputs>1)
            return fail("Only one WLED output is supported by the configured singleton WLED adapter");
        if((kind=="NetworkInput" || kind=="NetworkOutput") && !params.contains("data_type") && params.contains("type")) params["data_type"]=params["type"];
        const auto definition=node_definition(kind);
        for(auto it=definition.defaults.begin();it!=definition.defaults.end();++it) if(!params.contains(it.key())) params[it.key()]=it.value();
        for(auto it=definition.constraints.begin();it!=definition.constraints.end();++it) {
            const auto meta=it.value().toObject();const auto v=params.value(it.key());const auto type=meta.value("type").toString();
            if(v.isUndefined()) continue;
            if((type=="string" && !v.isString()) || (type=="bool" && !v.isBool()) ||
               ((type=="integer" || type=="number") && (!v.isDouble() || !std::isfinite(v.toDouble()))) ||
               (type=="integer" && std::floor(v.toDouble())!=v.toDouble()) ||
               (meta.contains("min") && v.toDouble()<meta.value("min").toDouble()) ||
               (meta.contains("max") && v.toDouble()>meta.value("max").toDouble())) return fail("Invalid parameter: "+kind+"."+it.key());
            if(type=="enum") {
                bool match=false;for(const auto &opt:meta.value("options").toArray()) if(opt.toObject().value("value")==v) match=true;
                if(!match) return fail("Invalid option: "+kind+"."+it.key());
            }
        }
        if (kind == "TestSignal") {
            TestSignalSettings settings;
            QString detail;
            if (!TestSignalSettings::parse(params, settings, detail)) return fail(detail);
        }
        if ((kind=="NetworkInput" || kind=="NetworkOutput") && params.value("stream_id").toString().trimmed().isEmpty()) return fail("Network stream_id must not be empty");
        // Relationships that cannot be expressed as scalar inspector constraints.
        const auto size=params.value("size").toInt();
        if(kind=="Window" && (size>int(dsp_core::max_fft_samples) || (size & (size-1)))) return fail("Window size must be a supported power of two");
        by_id.insert(id, {id, -1, kind, params});
    }
    for (const auto &value : connections) {
        const auto edge = value.toObject();
        const auto from = edge.value("outNodeId").toInt(-1), to = edge.value("inNodeId").toInt(-1);
        if (!by_id.contains(from) || !by_id.contains(to) || edge.value("inPortIndex").toInt(-1) != 0 ||
            edge.value("outPortIndex").toInt(-1) != 0)
            return fail("Connection refers to a missing node or port");
        auto port = [&](const Node &n, bool output) {
            if (n.kind == "NetworkInput" || n.kind == "NetworkOutput")
                return n.parameters.value("data_type").toString(n.parameters.value("type").toString("mel"));
            return output ? node_definition(n.kind).output : node_definition(n.kind).input;
        };
        const auto out = port(by_id[from],true), in = port(by_id[to],false);
        if (out.isEmpty() || in.isEmpty() || out != in)
            return fail("Incompatible connection: " + by_id[from].kind + " → " + by_id[to].kind);
        if (by_id[to].input != -1)
            return fail("An input can have only one connection");
        by_id[to].input = from;
    }
    auto program = std::make_shared<GraphProgram>();
    program->_document = canonical_graph_document(document);
    QSet<int> visiting, visited;
    std::function<bool(int)> visit = [&](int id) {
        if (visiting.contains(id)) {
            error = "Pipeline contains a cycle";
            return false;
        }
        if (visited.contains(id))
            return true;
        auto &node = by_id[id];
        visiting.insert(id);
        if (node.input != -1 && !visit(node.input))
            return false;
        visiting.remove(id);
        visited.insert(id);
        program->_ordered.push_back(node);
        return true;
    };
    for (auto id : by_id.keys())
        if (!visit(id))
            return {};
    for (size_t i = 0; i < program->_ordered.size(); ++i) {
        auto &node = program->_ordered[i];
        node.source = node.kind == "TestSignal" ? 2 : node.parameters.value("source").toInt(0);
        node.channel = node.parameters.value("channel").toInt(0);
        node.history_size = node.parameters.value("history_size").toInt(400);
        node.merge = static_cast<SpectrumMerge>(node.parameters.value("merge").toInt(2));
        node.gain = static_cast<float>(node.parameters.value("gain").toDouble(1));
        node.enabled=node.parameters.value("enabled").toBool(true);
        node.active=node.enabled;
        if(node.input!=-1) for(const auto &parent:program->_ordered) if(parent.id==node.input) {node.active=node.active && parent.active;break;}
    }
    return program;
}
QJsonObject GraphProgram::test_signal_defaults() {
    auto document = defaults();
    auto nodes = document["nodes"].toArray();
    auto node = nodes[0].toObject();
    auto parameters = TestSignalSettings{}.json();
    parameters["model-name"] = "TestSignal";
    node["internal-data"] = parameters; nodes[0] = node; document["nodes"] = nodes;
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
    if(graph_id()!=other.graph_id()) return false;
    if (_ordered.size() != other._ordered.size())
        return false;
    for (size_t i = 0; i < _ordered.size(); ++i) {
        const auto &a = _ordered[i], &b = other._ordered[i];
        if (a.id != b.id || a.input != b.input || a.kind != b.kind || a.parameters != b.parameters)
            return false;
    }
    return true;
}
namespace {
QString frame_type_name(FrameType type) {
    switch(type) {
    case FrameType::PCM: return "audio"; case FrameType::Window: return "window";
    case FrameType::Windowed: return "windowed"; case FrameType::StereoSpectrum: return "stereo-spectrum";
    case FrameType::Spectrum: return "spectrum"; case FrameType::Mel: return "mel"; case FrameType::History: return "history";
    }
    return {};
}
void weight(TypedFrame &f, std::vector<float> &weights) {
    if (f.bark_weighted) return;
    weights.resize(f.left.size()); dsp_core::bark_weights(weights, f.frequency_step);
    for(size_t i=0;i<weights.size();++i) { f.left[i]*=weights[i]; f.right[i]*=weights[i]; }
    f.bark_weighted=true;
}
void merge(TypedFrame &f, SpectrumMerge mode) {
    f.values = Analyzer::merge_spectra(f.left,f.right,mode); f.type=FrameType::Spectrum;
    auto peak=std::max_element(f.values.begin(),f.values.end());
    f.magnitude=peak==f.values.end()?0:*peak;
    f.peak_hz=peak==f.values.end()?0:std::distance(f.values.begin(),peak)*f.frequency_step;
}
}
std::vector<int> GraphProgram::ingress_nodes() const {
    std::vector<int> ids;
    for(const auto &n:_ordered) if(node_definition(n.kind).input.isEmpty()) ids.push_back(n.id);
    return ids;
}
QString GraphProgram::port_type(int id) const {
    for(const auto &n:_ordered) if(n.id==id) {
        if(n.kind=="NetworkInput" || n.kind=="NetworkOutput") return n.parameters.value("data_type").toString(n.parameters.value("type").toString("mel"));
        return node_definition(n.kind).output;
    }
    return {};
}
std::vector<int> GraphProgram::clear_ingress(int ingress, GraphEvaluationContext &context) const {
    if(context._program!=this) { context._states.clear();context._program=this; }
    std::vector<int> affected;
    for(const auto &node:_ordered) if(node.id==ingress || std::find(affected.begin(),affected.end(),node.input)!=affected.end()) {
        affected.push_back(node.id);auto &state=context._states[node.id];
        state.history.clear();state.pcm_left.clear();state.pcm_right.clear();state.progress=state.remainder=state.hop=0;
        state.unavailable=true;
        state.pending_discontinuity=true;
    }
    return affected;
}
GraphOutput GraphProgram::disabled_output(bool previews) const {
    GraphOutput result;
    for(const auto &node:_ordered) if(!node.active) {
        const auto definition=node_definition(node.kind);
        const auto type=definition.category=="views" || node.kind=="WLED"?definition.input:port_type(node.id);
        TypedFrame marker;marker.available=false;marker.discontinuity=true;
        marker.type=type=="window"?FrameType::Window:type=="windowed"?FrameType::Windowed:
            type=="stereo-spectrum"?FrameType::StereoSpectrum:type=="spectrum"?FrameType::Spectrum:
            type=="mel"?FrameType::Mel:type=="history"?FrameType::History:FrameType::PCM;
        marker.stream_id=node.parameters.value("stream_id").toString().toStdString();
        if(definition.category=="views") result.views[node.id]=marker;
        else result.ports[node.id]=marker;
        if(definition.category=="outputs") result.output_events.emplace_back(node.id,marker);
        if(previews) result.previews[node.id]={};
    }
    return result;
}
std::vector<int> GraphProgram::rebind(GraphEvaluationContext &context, const GraphProgram &previous,
                                     uint64_t previous_epoch, uint64_t next_epoch) const {
    QSet<int> reset, retained;
    auto dimensions=[](const Node &node) {
        QJsonObject result{{"kind",node.kind}};
        if(node.kind=="Window") result["size"]=node.parameters["size"];
        if(node.kind=="Mel") result["bands"]=node.parameters["bands"];
        if(node.kind.startsWith("Network")) result["data_type"]=node.parameters["data_type"];
        if(node_definition(node.kind).input.isEmpty()) {
            QJsonObject parameters;
            const auto defaults=node_definition(node.kind).defaults;
            for(auto it=defaults.begin();it!=defaults.end();++it) if(it.key()!="enabled") parameters[it.key()]=node.parameters[it.key()];
            result["source"]=parameters;
        }
        return result;
    };
    for(const auto &node:_ordered) {
        retained.insert(node.id);
        const auto old=std::find_if(previous._ordered.begin(),previous._ordered.end(),[&](const Node &n){return n.id==node.id;});
        if(graph_id()!=previous.graph_id() || old==previous._ordered.end() || dimensions(node)!=dimensions(*old) || reset.contains(node.input)) {
            context._states.remove(node.id);reset.insert(node.id);continue;
        }
        auto it=context._states.find(node.id);if(it==context._states.end()) continue;
        if(node.kind=="WindowFunction" && node.parameters["function"]!=old->parameters["function"]) it->hann.clear();
        if(node.kind=="Spectrogram" && it->history.size()>size_t(node.history_size))
            it->history.erase(it->history.begin(),it->history.end()-node.history_size);
        // Receiver rebinding is separate from a remote producer incarnation.
        const auto clock_source=std::find_if(_ordered.begin(),_ordered.end(),[&](const Node &n){return n.id==it->clock_ingress;});
        const bool local_clock=clock_source!=_ordered.end() && (clock_source->kind=="AudioSource" || clock_source->kind=="TestSignal");
        if(local_clock && previous_epoch && it->producer_epoch==previous_epoch) {
            it->producer_epoch=next_epoch;it->input_template.producer_epoch=next_epoch;
            it->input_template.epoch=next_epoch;
            if(node_definition(node.kind).input.isEmpty()) {it->sequence=0;it->preserve_next_tick=true;}
        }
    }
    for(auto it=context._states.begin();it!=context._states.end();) {
        if(!retained.contains(it.key())) it=context._states.erase(it);else ++it;
    }
    context._program=this;
    return std::vector<int>(reset.begin(),reset.end());
}
bool GraphProgram::marker_current(int ingress, const TypedFrame &frame, const GraphEvaluationContext &context) const {
    const auto it=context._states.constFind(ingress);
    if(it==context._states.cend()) return true;
    const auto incarnation=frame.epoch;
    if(std::find(it->retired_epochs.begin(),it->retired_epochs.end(),incarnation)!=it->retired_epochs.end()) return false;
    return incarnation!=it->producer_epoch || !frame.generation || frame.generation>=it->generation;
}
GraphOutput GraphProgram::evaluate(const SpectrumFrame &f, bool previews) const {
    GraphEvaluationContext c; return evaluate(f,c,previews);
}
GraphOutput GraphProgram::evaluate(const SpectrumFrame &f, GraphEvaluationContext &c, bool previews) const {
    // Compatibility ingress for callers already supplying the legacy weighted FFT.
    TypedFrame input; input.type=FrameType::StereoSpectrum; input.left=f.left; input.right=f.right;
    input.sample_rate=f.sample_rate; input.frequency_step=f.frequency_step?f.frequency_step:float(f.sample_rate)/(2*f.left.size());
    input.fft_size=f.left.size()*2; input.bark_weighted=true; input.timestamp_ns=f.timestamp_ns;
    input.sequence=f.sequence; input.generation=f.generation; input.discontinuity=f.discontinuity;
    auto ids=ingress_nodes(); return ids.empty()?GraphOutput{}:evaluate(ids.front(),input,c,previews);
}
GraphOutput GraphProgram::evaluate(int ingress, const TypedFrame &input, GraphEvaluationContext &context, bool previews) const {
    if(context._program!=this) { context._states.clear(); context._program=this; }
    GraphOutput result;
    if(!input.available) return result; // Unavailable is a runtime control event, never DSP silence.
    const auto ingress_node=std::find_if(_ordered.begin(),_ordered.end(),[&](const auto &n){return n.id==ingress && node_definition(n.kind).input.isEmpty();});
    if(ingress_node==_ordered.end()) return result;
    auto &origin=context._states[ingress];
    const uint64_t incarnation=input.producer_epoch?input.producer_epoch:input.epoch;
    const bool new_incarnation=incarnation!=origin.producer_epoch;
    if(new_incarnation && std::find(origin.retired_epochs.begin(),origin.retired_epochs.end(),incarnation)!=origin.retired_epochs.end()) return result;
    if(!new_incarnation && (input.generation<origin.generation || (input.generation==origin.generation && input.sequence && origin.sequence && input.sequence<=origin.sequence && !input.discontinuity))) return result;
    if(new_incarnation) {
        if(origin.producer_epoch) origin.retired_epochs.push_back(origin.producer_epoch);
        if(origin.retired_epochs.size()>16) origin.retired_epochs.erase(origin.retired_epochs.begin());
    }
    QMap<int,std::vector<TypedFrame>> values;
    // A disconnected node retains its former upstream clock. Order that clock
    // before it even when the current topology no longer contains the edge.
    std::vector<const Node *> schedule;QSet<int> scheduled,scheduling;
    std::function<void(const Node &)> schedule_node=[&](const Node &node) {
        if(scheduled.contains(node.id) || scheduling.contains(node.id)) return;
        scheduling.insert(node.id);
        const auto saved=context._states.constFind(node.id);
        const int clock=node.input>=0?node.input:saved==context._states.cend()?-1:saved->clock_node;
        for(const auto &parent:_ordered) if(parent.id==clock) {schedule_node(parent);break;}
        scheduling.remove(node.id);scheduled.insert(node.id);schedule.push_back(&node);
    };
    for(const auto &node:_ordered) schedule_node(node);
    for(const auto *scheduled_node:schedule) {
        const auto &node=*scheduled_node;
        const auto &kind=node.kind;
        const bool source=node_definition(kind).input.isEmpty();
        if(source && node.id!=ingress) continue;
        auto &node_state=context._states[node.id];
        const bool detached=!source && node.input<0;
        std::vector<TypedFrame> inputs;
        if(source) inputs={input};
        else if(values.contains(node.input)) inputs=values[node.input];
        else if(detached && node_state.has_template && node_state.clock_ingress==ingress) {
            const auto clocks=values.contains(node_state.clock_node)?values[node_state.clock_node]:std::vector<TypedFrame>{input};
            for(const auto &clock:clocks) {
                auto zero=node_state.input_template;zero.sequence=clock.sequence;zero.source_sequence=clock.source_sequence;
                zero.timestamp_ns=clock.timestamp_ns;zero.epoch=clock.epoch;zero.producer_epoch=clock.producer_epoch;
                zero.generation=clock.generation;zero.discontinuity=false;inputs.push_back(std::move(zero));
            }
        } else {
            if(previews && (detached || !node.active)) result.previews[node.id]={};
            continue;
        }
        for(auto f:inputs) {
        if(source && kind=="NetworkInput" && frame_type_name(f.type)!=port_type(node.id)) continue;
        auto &state=context._states[node.id];
        if(source && state.preserve_next_tick) {f.discontinuity=false;state.preserve_next_tick=false;}
        state.clock_ingress=ingress;if(node.input>=0) state.clock_node=node.input;
        state.input_template=f;state.has_template=true;
        if(detached || !node.enabled) {
            std::fill(f.left.begin(),f.left.end(),0);std::fill(f.right.begin(),f.right.end(),0);std::fill(f.values.begin(),f.values.end(),0);
            if(!f.history.empty()) std::fill(f.history.back().begin(),f.history.back().end(),0);
            f.peak_hz=f.magnitude=0;f.available=true;
        }
        const bool broken=state.unavailable || f.discontinuity || state.producer_epoch!=incarnation || state.generation!=f.generation || (state.rate && state.rate!=f.sample_rate) ||
            (source && state.sequence && f.sequence && f.sequence!=state.sequence+1);
        state.producer_epoch=incarnation;f.producer_epoch=incarnation;state.unavailable=false;
        if(broken) { state.history.clear(); state.pcm_left.clear();state.pcm_right.clear();state.progress=state.remainder=state.hop=0;state.pending_discontinuity=true; }
        state.generation=f.generation; state.sequence=f.sequence;
        const auto previous_rate=state.rate;state.rate=f.sample_rate;
        if(broken) f.discontinuity=true;
        if(kind=="Window" && f.type==FrameType::PCM) {
            const size_t n=node.parameters.value("size").toInt(4096);
            if(state.pcm_left.size()!=n) { state.pcm_left.assign(n,0); state.pcm_right.assign(n,0); }
            size_t offset=0;
            while(offset<f.left.size()) {
                if(!state.hop) { state.hop=std::max(size_t{1},f.sample_rate/60);state.remainder+=f.sample_rate%60;
                    if(state.remainder>=60) { ++state.hop;state.remainder-=60; } }
                const size_t count=std::min(f.left.size()-offset,state.hop-state.progress);
                const size_t kept=std::min(n,count);
                std::move(state.pcm_left.begin()+kept,state.pcm_left.end(),state.pcm_left.begin());
                std::move(state.pcm_right.begin()+kept,state.pcm_right.end(),state.pcm_right.begin());
                std::copy(f.left.begin()+offset+count-kept,f.left.begin()+offset+count,state.pcm_left.end()-kept);
                std::copy(f.right.begin()+offset+count-kept,f.right.begin()+offset+count,state.pcm_right.end()-kept);
                offset+=count;state.progress+=count;
                if(state.progress==state.hop) {
                    auto window=f;window.left=state.pcm_left;window.right=state.pcm_right;window.type=FrameType::Window;window.fft_size=n;
                    window.timestamp_ns=f.timestamp_ns+int64_t(offset*1000000000ULL/f.sample_rate);
                    window.source_sequence=f.source_sequence?f.source_sequence:f.sequence;window.sequence=++state.output_sequence;
                    window.discontinuity=f.discontinuity || state.pending_discontinuity;state.pending_discontinuity=false;
                    if(!node.enabled) {std::fill(window.left.begin(),window.left.end(),0);std::fill(window.right.begin(),window.right.end(),0);}
                    ++result.windows_processed;
                    values[node.id].push_back(window);result.ports[node.id]=window;
                    f.discontinuity=false;state.progress=state.hop=0;
                }
            }
            continue;
        } else if(kind=="Window" && f.type==FrameType::Window) {
            const size_t n=node.parameters.value("size").toInt(4096);
            if(f.left.size()<n) continue;
            if(f.left.size()!=n) { f.left.erase(f.left.begin(),f.left.end()-n);f.right.erase(f.right.begin(),f.right.end()-n); }
            f.fft_size=n;
        } else if(kind=="WindowFunction") {
            if(f.type==FrameType::StereoSpectrum) { values[node.id].push_back(f);continue; }
            if(f.type!=FrameType::Window) continue;
            if(state.hann.size()!=f.left.size()) {
                state.hann.resize(f.left.size());const auto name=node.parameters.value("function").toString("hann");
                dsp_core::window(state.hann,name=="hamming"?dsp_core::WindowFunction::Hamming:name=="blackman"?dsp_core::WindowFunction::Blackman:name=="rectangular"?dsp_core::WindowFunction::Rectangular:dsp_core::WindowFunction::Hann);
            }
            for(size_t i=0;i<f.left.size();++i) { f.left[i]*=state.hann[i];f.right[i]*=state.hann[i]; }
            f.type=FrameType::Windowed;
        } else if(kind=="StereoFFT") {
            if(f.type!=FrameType::StereoSpectrum) {
                if(f.type!=FrameType::Windowed) continue;
                const size_t n=f.left.size();
                if(state.bins!=n) { if(!state.fft.prepare(n,(n & 0x55555555)!=0)) continue; state.bins=n; }
                std::vector<float> l(n/2),r(n/2);
                if(!state.fft.process(f.left,l) || !state.fft.process(f.right,r)) continue;
                f.left=std::move(l);f.right=std::move(r);f.type=FrameType::StereoSpectrum;
                f.frequency_step=float(f.sample_rate)/n;f.fft_size=n;f.bark_weighted=false;
            }
        } else if(kind=="BarkWeighting") {
            weight(f,state.weights);
        } else if(kind=="SpectrumMerge") {
            merge(f,node.merge);
            f.left.clear();f.right.clear();
        } else if(kind=="SpectrumGain") {
            for(auto &v:f.values) v*=node.gain;
            f.magnitude*=node.gain;
        } else if(kind=="Mel") {
            const size_t bands=node.parameters.value("bands").toInt(50);
            if(state.bins!=f.values.size() || previous_rate!=f.sample_rate || state.bands!=bands) {
                state.mel.prepare(f.values.size(),f.sample_rate,bands);state.bins=f.values.size();state.bands=bands;
            }
            std::vector<float> mel(bands);state.mel.process(f.values,mel);f.values=std::move(mel);f.type=FrameType::Mel;f.bands=bands;
        } else if(kind=="Spectrogram") {
            // External Mel ingress need not provide redundant bands metadata.
            // The actual row width defines this history's structural contract.
            if(!state.history.empty() && state.history.back().size()!=f.values.size()) {
                state.history.clear();f.discontinuity=true;
            }
            f.bands=f.values.size();
            state.history.push_back(f.values);
            if(state.history.size()>size_t(node.history_size)) state.history.erase(state.history.begin());
            f.history=state.history;f.type=FrameType::History;f.history_capacity=node.history_size;
            result.history=f.values;result.history_size=node.history_size;result.history_events.emplace_back(node.id,f.values);
        } else if(kind=="FFTView") {
            if(node.channel==2) { merge(f,node.merge);f.type=FrameType::StereoSpectrum; }
            else f.values=node.channel==0?f.left:f.right;
            result.fft=f.values;
        } else if(kind=="MelView") result.mel=f.values;
        else if(kind=="SpectrogramView") result.spectrogram=true;
        else if(kind=="WLED") {
            for(auto &v:f.values) v*=node.gain;result.network=f.values;
            result.network_node=node.id;
            result.peak_hz=f.peak_hz;result.magnitude=f.magnitude;
        }
        state.rate=f.sample_rate;
        values[node.id].push_back(f);
        const auto definition=node_definition(kind);
        if(definition.category=="views") result.views[node.id]=f;
        if(kind=="NetworkOutput") f.stream_id=node.parameters.value("stream_id").toString().toStdString();
        if(!definition.output.isEmpty() || definition.category=="outputs") result.ports[node.id]=f;
        if(definition.category=="outputs") result.output_events.emplace_back(node.id,f);
        if(previews) {
            const auto &v=!f.values.empty()?f.values:f.left;
            float peak=0;for(auto x:v) peak=std::max(peak,std::abs(x));
            result.previews[node.id]={v.size(),peak};
        }
    }
    }
    return result;
}
} // namespace pipeline
