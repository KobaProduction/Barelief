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
    static const std::vector<NodeDefinition> definitions{{"AudioSource", "", "audio"},
                                                         {"StereoFFT", "audio", "stereo-spectrum"},
                                                         {"SpectrumMerge", "stereo-spectrum", "spectrum"},
                                                         {"SpectrumGain", "spectrum", "spectrum"},
                                                         {"Mel", "spectrum", "mel"},
                                                         {"Spectrogram", "mel", "history"},
                                                         {"FFTView", "stereo-spectrum", ""},
                                                         {"MelView", "mel", ""},
                                                         {"SpectrogramView", "history", ""},
                                                         {"AudioView", "audio", ""},
                                                         {"WLED", "mel", ""},
                                                         {"TestSignal", "", "audio"}};
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
    return {{"version", 1}, {"layout_pending", true}, {"nodes", nodes}, {"connections", edges}};
}
std::shared_ptr<const GraphProgram> GraphProgram::compile(const QJsonObject &document, QString &error) {
    error.clear();
    auto fail = [&](QString message) -> std::shared_ptr<const GraphProgram> {
        error = message;
        return {};
    };
    if (document.value("version").toInt(-1) != 1)
        return fail("Unsupported pipeline version (expected 1)");
    const auto nodes = document.value("nodes").toArray();
    const auto connections = document.value("connections").toArray();
    if (nodes.isEmpty() || nodes.size() > 128 || connections.size() > 256)
        return fail("Pipeline must contain 1–128 nodes and at most 256 connections");
    QMap<int, Node> by_id;
    QMap<QString, int> counts;
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
        if (kind == "TestSignal") {
            TestSignalSettings settings;
            QString detail;
            if (!TestSignalSettings::parse(params, settings, detail)) return fail(detail);
        }
        auto integral_parameter = [&](const QString &key) {
            if (!params.contains(key))
                return true;
            const auto value = params.value(key);
            return value.isDouble() && std::isfinite(value.toDouble()) &&
                   std::floor(value.toDouble()) == value.toDouble();
        };
        if ((kind == "AudioSource" && !integral_parameter("source")) ||
            ((kind == "SpectrumMerge" || kind == "FFTView") && !integral_parameter("merge")) ||
            (kind == "FFTView" && !integral_parameter("channel")) ||
            (kind == "Spectrogram" && !integral_parameter("history_size")))
            return fail("Invalid parameter type: " + kind);
        if ((kind == "SpectrumGain" || kind == "WLED") && params.contains("gain") && !params.value("gain").isDouble())
            return fail("Gain must be numeric");
        if (kind == "AudioSource" && (params.value("source").toInt(0) < 0 || params.value("source").toInt(0) > 1))
            return fail("Invalid audio source");
        if (kind == "SpectrumMerge" || kind == "FFTView") {
            const auto mode = params.value("merge").toInt(2);
            if (mode < 0 || mode > 2)
                return fail("Invalid merge mode");
        }
        if (kind == "FFTView" && (params.value("channel").toInt(0) < 0 || params.value("channel").toInt(0) > 2))
            return fail("Invalid FFT channel");
        if (kind == "SpectrumGain" || kind == "WLED") {
            const auto gain = params.value("gain").toDouble(1);
            if (!std::isfinite(gain) || gain < 0 || gain > 100)
                return fail("Gain must be between 0 and 100");
        }
        if (kind == "Spectrogram" &&
            (params.value("history_size").toInt(400) < 2 || params.value("history_size").toInt(400) > 2000))
            return fail("History must contain 2–2000 frames");
        by_id.insert(id, {id, -1, kind, params});
        ++counts[kind];
    }
    if (counts["AudioSource"] + counts["TestSignal"] != 1 || counts["StereoFFT"] != 1)
        return fail("Exactly one audio source and one stereo FFT are required");
    for (const auto &kind : {"FFTView", "MelView", "SpectrogramView", "AudioView", "Spectrogram", "WLED"})
        if (counts[kind] > 1)
            return fail(QString("Only one %1 is supported").arg(kind));
    for (const auto &value : connections) {
        const auto edge = value.toObject();
        const auto from = edge.value("outNodeId").toInt(-1), to = edge.value("inNodeId").toInt(-1);
        if (!by_id.contains(from) || !by_id.contains(to) || edge.value("inPortIndex").toInt(-1) != 0 ||
            edge.value("outPortIndex").toInt(-1) != 0)
            return fail("Connection refers to a missing node or port");
        const auto out = node_definition(by_id[from].kind).output, in = node_definition(by_id[to].kind).input;
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
        if (!node_definition(node.kind).input.isEmpty() && node.input == -1) {
            error = "Missing input: " + node.kind;
            return false;
        }
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
        const auto &definitions = node_definitions();
        node.operation = static_cast<Operation>(
            std::distance(definitions.begin(), std::find_if(definitions.begin(), definitions.end(),
                                                            [&](const auto &d) { return d.name == node.kind; })));
        if (node.kind == "TestSignal") node.operation = Operation::AudioSource;
        node.source = node.kind == "TestSignal" ? 2 : node.parameters.value("source").toInt(0);
        node.channel = node.parameters.value("channel").toInt(0);
        node.history_size = node.parameters.value("history_size").toInt(400);
        node.merge = static_cast<SpectrumMerge>(node.parameters.value("merge").toInt(2));
        node.gain = static_cast<float>(node.parameters.value("gain").toDouble(1));
        if (node.input != -1)
            for (size_t j = 0; j < i; ++j)
                if (program->_ordered[j].id == node.input)
                    node.input_slot = static_cast<int>(j);
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
        if (node.operation == Operation::AudioSource)
            return node.source;
    return 0;
}
QJsonObject GraphProgram::source_parameters() const {
    for (const auto &node : _ordered)
        if (node.kind == "TestSignal") {
            TestSignalSettings settings;
            QString error;
            TestSignalSettings::parse(node.parameters, settings, error);
            return settings.json();
        }
    return {};
}
bool GraphProgram::contains(const QString &kind) const {
    return std::any_of(_ordered.begin(), _ordered.end(), [&](const auto &node) { return node.kind == kind; });
}
bool GraphProgram::same_processing_as(const GraphProgram &other) const {
    if (_ordered.size() != other._ordered.size())
        return false;
    for (size_t i = 0; i < _ordered.size(); ++i) {
        const auto &a = _ordered[i], &b = other._ordered[i];
        if (a.id != b.id || a.input != b.input || a.kind != b.kind || a.parameters != b.parameters)
            return false;
    }
    return true;
}
GraphOutput GraphProgram::evaluate(const SpectrumFrame &frame, bool previews) const {
    GraphEvaluationContext context;
    return evaluate(frame, context, previews);
}
GraphOutput GraphProgram::evaluate(const SpectrumFrame &frame, GraphEvaluationContext &context, bool previews) const {
    auto &values = context._values;
    values.resize(_ordered.size());
    auto merge_into = [&](std::vector<float> &out, SpectrumMerge mode) {
        const auto n = std::min(frame.left.size(), frame.right.size());
        out.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const auto l = frame.left[i], r = frame.right[i];
            out[i] = mode == SpectrumMerge::Mean      ? (l + r) * 0.5f
                     : mode == SpectrumMerge::Maximum ? std::max(l, r)
                                                      : std::hypot(l, r) / std::sqrt(2.0f);
        }
    };
    size_t slot = 0;
    GraphOutput result;
    for (const auto &node : _ordered) {
        auto &value = values[slot++];
        if (node.input_slot == -1) {
            value.spectrum.clear();
            value.mel.clear();
            value.hz = 0;
            value.magnitude = 0;
            value.history = 400;
        } else {
            const auto &input = values[static_cast<size_t>(node.input_slot)];
            value.spectrum.assign(input.spectrum.begin(), input.spectrum.end());
            value.mel.assign(input.mel.begin(), input.mel.end());
            value.hz = input.hz;
            value.magnitude = input.magnitude;
            value.history = input.history;
        }
        const auto kind = node.operation;
        if (kind == Operation::SpectrumMerge) {
            merge_into(value.spectrum, node.merge);
            const auto peak = std::max_element(value.spectrum.begin(), value.spectrum.end());
            value.magnitude = peak == value.spectrum.end() ? 0 : *peak;
            value.hz = peak == value.spectrum.end() ? 0
                                                    : std::distance(value.spectrum.begin(), peak) *
                                                          float(frame.sample_rate) / (2 * value.spectrum.size());
        } else if (kind == Operation::SpectrumGain) {
            const auto gain = node.gain;
            for (auto &v : value.spectrum)
                v *= gain;
            value.magnitude *= gain;
        } else if (kind == Operation::Mel) {
            if (context._mel_bins != value.spectrum.size() || context._mel_rate != frame.sample_rate) {
                context._mel_filter.prepare(value.spectrum.size(), frame.sample_rate, 50);
                context._mel_bins = value.spectrum.size();
                context._mel_rate = frame.sample_rate;
            }
            value.mel.resize(50);
            context._mel_filter.process(value.spectrum, value.mel);
        } else if (kind == Operation::Spectrogram) {
            value.history = node.history_size;
            result.history = value.mel;
            result.history_size = value.history;
        } else if (kind == Operation::FFTView) {
            const auto channel = node.channel;
            if (channel == 0)
                result.fft = frame.left;
            else if (channel == 1)
                result.fft = frame.right;
            else
                merge_into(result.fft, node.merge);
        } else if (kind == Operation::MelView)
            result.mel = value.mel;
        else if (kind == Operation::SpectrogramView)
            result.spectrogram = true;
        else if (kind == Operation::WLED) {
            result.network = value.mel;
            const auto gain = node.gain;
            for (auto &v : result.network)
                v *= gain;
            result.peak_hz = value.hz;
            result.magnitude = value.magnitude;
        }
        if (previews) {
            const auto &preview = kind == Operation::FFTView ? result.fft
                                  : !value.mel.empty()       ? value.mel
                                  : !value.spectrum.empty()  ? value.spectrum
                                                             : frame.left;
            result.previews[node.id] = {preview.size(),
                                        preview.empty() ? 0 : *std::max_element(preview.begin(), preview.end())};
            if (kind == Operation::AudioSource || kind == Operation::AudioView)
                result.previews[node.id] = {frame.audio_samples, frame.audio_peak};
            if (kind == Operation::StereoFFT) {
                float peak = 0;
                for (auto v : frame.left)
                    peak = std::max(peak, v);
                for (auto v : frame.right)
                    peak = std::max(peak, v);
                result.previews[node.id] = {frame.left.size() + frame.right.size(), peak};
            }
        }
    }
    return result;
}
} // namespace pipeline
