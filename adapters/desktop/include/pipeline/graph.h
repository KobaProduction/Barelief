#pragma once
#include "pipeline/engine_models.h"
#include "analyzer/analyze/analyzer.h"
#include "pipeline/typed_frame.h"
#include <dataflow_runtime/runtime.h>
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
    std::shared_ptr<const dataflow_runtime::PreparedGraph> _plan;
    std::unique_ptr<dataflow_runtime::RuntimeContext> _runtime;
    std::vector<dataflow_runtime::IngressSpec> _ingress;
    application::RuntimeLimits _limits;
    QMap<int, uint64_t> _legacy_sequence;
    const GraphProgram *_program = nullptr;

  public:
    GraphEvaluationContext() = default;
    GraphEvaluationContext(const GraphEvaluationContext &);
    GraphEvaluationContext &operator=(const GraphEvaluationContext &);
    GraphEvaluationContext(GraphEvaluationContext &&) noexcept = default;
    GraphEvaluationContext &operator=(GraphEvaluationContext &&) noexcept = default;
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
    std::vector<int> rebind(GraphEvaluationContext &context, const GraphProgram &previous, uint64_t previous_epoch = 0,
                            uint64_t next_epoch = 0, signal_contracts::Status *status = nullptr) const;
    QString graph_id() const {
        return _document.value("graph_id").toString("main");
    }
    std::vector<int> clear_ingress(int ingress_node, GraphEvaluationContext &context) const;
    bool marker_current(int ingress_node, const TypedFrame &frame, const GraphEvaluationContext &context) const;
    GraphOutput evaluate(int ingress_node, const TypedFrame &frame, GraphEvaluationContext &context,
                         bool previews = true) const;
    GraphOutput evaluate(const SpectrumFrame &frame, bool previews = true) const;
    GraphOutput evaluate(const SpectrumFrame &frame, GraphEvaluationContext &context, bool previews = true) const;
    // Negotiation/preparation may allocate. Run it before entering process_view.
    signal_contracts::Status prepare_ingress(int node, const signal_contracts::FrameView &,
                                             GraphEvaluationContext &) const;
    signal_contracts::Status process_view(int node, const signal_contracts::FrameView &, GraphEvaluationContext &,
                                          dataflow_runtime::OutputCallback, void *user = nullptr) const noexcept;
    std::vector<std::vector<float>> history_snapshot(int node, const GraphEvaluationContext &) const;
    int history_ingress(int node, const GraphEvaluationContext &) const noexcept;
    void reset_history(int node, GraphEvaluationContext &) const noexcept;
    void rebase_local_producers(GraphEvaluationContext &, uint64_t previous_epoch, uint64_t next_epoch,
                                uint64_t generation) const noexcept;
    static signal_contracts::FrameView frame_view(const TypedFrame &, std::span<const float> history_storage = {});
    static TypedFrame owned_snapshot(const signal_contracts::FrameView &);
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
    domain::GraphDefinition _definition;
};
} // namespace pipeline
