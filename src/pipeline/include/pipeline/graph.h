#pragma once
#include "analyzer/analyze/analyzer.h"
#include "pipeline/typed_frame.h"
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <memory>
namespace pipeline {
class GraphProgram;
struct NodeDefinition {
    QString name, input, output;
    QString category;
    QJsonObject defaults, constraints;
};
const std::vector<NodeDefinition> &node_definitions();
NodeDefinition node_definition(const QString &name);
QString canonical_node_name(const QString &name);
QJsonObject canonical_graph_document(QJsonObject document);
struct NodePreview {
    size_t values = 0;
    float peak = 0;
};
struct GraphOutput {
    QMap<int, TypedFrame> ports, views;
    std::vector<std::pair<int, TypedFrame>> output_events;
    std::vector<std::pair<int, std::vector<float>>> history_events;
    size_t windows_processed = 0;
    int network_node = -1;
    std::vector<float> fft, mel, history, network;
    float peak_hz = 0, magnitude = 0;
    int history_size = 400;
    bool spectrogram = false;
    QMap<int, NodePreview> previews;
};
// One context per evaluator; the owner must serialize its use. It can move to a
// new worker after the previous worker joins. The compiled graph stays immutable.
class GraphEvaluationContext final {
    friend class GraphProgram;
    friend class AudioPipeline;
    struct State {
        dsp_core::FFT fft;
        dsp_core::MelFilterBank mel;
        std::vector<float> hann, weights, pcm_left, pcm_right;
        std::vector<std::vector<float>> history;
        size_t bins = SIZE_MAX, rate = 0, progress = 0, remainder = 0, hop = 0;
        size_t bands = 0;
        float frequency_step = -1;
        uint64_t generation = 0, sequence = 0;
        uint64_t output_sequence = 0;
        uint64_t producer_epoch = 0;
        std::vector<uint64_t> retired_epochs;
        bool unavailable = false;
        bool pending_discontinuity = false;
        TypedFrame input_template;
        int clock_ingress = -1, clock_node = -1;
        bool has_template = false;
        bool preserve_next_tick = false;
    };
    QMap<int, State> _states;
    const GraphProgram *_program = nullptr;
};
class GraphProgram final {
  public:
    static QJsonObject defaults(int channel = 0, SpectrumMerge merge = SpectrumMerge::Energy, int source = 0);
    static QJsonObject test_signal_defaults();
    static std::shared_ptr<const GraphProgram> compile(const QJsonObject &document, QString &error);
    bool contains(const QString &kind) const;
    int capture_source() const;
    QJsonObject source_parameters() const;
    bool same_processing_as(const GraphProgram &other) const;
    std::vector<int> ingress_nodes() const;
    QString port_type(int node_id) const;
    GraphOutput disabled_output(bool previews = true) const;
    std::vector<int> rebind(GraphEvaluationContext &context, const GraphProgram &previous,
                            uint64_t previous_epoch = 0, uint64_t next_epoch = 0) const;
    QString graph_id() const { return _document.value("graph_id").toString("main"); }
    std::vector<int> clear_ingress(int ingress_node, GraphEvaluationContext &context) const;
    bool marker_current(int ingress_node, const TypedFrame &frame, const GraphEvaluationContext &context) const;
    GraphOutput evaluate(int ingress_node, const TypedFrame &frame, GraphEvaluationContext &context, bool previews = true) const;
    GraphOutput evaluate(const SpectrumFrame &frame, bool previews = true) const;
    GraphOutput evaluate(const SpectrumFrame &frame, GraphEvaluationContext &context, bool previews = true) const;
    const QJsonObject &document() const {
        return _document;
    }

  private:
    struct Node {
        int id, input = -1;
        QString kind;
        QJsonObject parameters;
        int source = 0, channel = 0, history_size = 400;
        SpectrumMerge merge = SpectrumMerge::Energy;
        float gain = 1;
        bool enabled = true, active = true;
    };
    QJsonObject _document;
    std::vector<Node> _ordered;
};
} // namespace pipeline
