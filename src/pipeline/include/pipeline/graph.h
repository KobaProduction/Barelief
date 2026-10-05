#pragma once
#include "analyzer/analyze/analyzer.h"
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <memory>
namespace pipeline {
struct NodeDefinition {
    QString name, input, output;
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
    struct Value {
        std::vector<float> spectrum, mel;
        float hz = 0, magnitude = 0;
        int history = 400;
    };
    std::vector<Value> _values;
    dsp_core::MelFilterBank _mel_filter;
    size_t _mel_bins = SIZE_MAX, _mel_rate = SIZE_MAX;
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
    GraphOutput evaluate(const SpectrumFrame &frame, bool previews = true) const;
    GraphOutput evaluate(const SpectrumFrame &frame, GraphEvaluationContext &context, bool previews = true) const;
    const QJsonObject &document() const {
        return _document;
    }

  private:
    enum class Operation {
        AudioSource,
        StereoFFT,
        SpectrumMerge,
        SpectrumGain,
        Mel,
        Spectrogram,
        FFTView,
        MelView,
        SpectrogramView,
        AudioView,
        WLED
    };
    struct Node {
        int id, input = -1;
        QString kind;
        QJsonObject parameters;
        Operation operation{};
        int input_slot = -1, source = 0, channel = 0, history_size = 400;
        SpectrumMerge merge = SpectrumMerge::Energy;
        float gain = 1;
    };
    QJsonObject _document;
    std::vector<Node> _ordered;
};
} // namespace pipeline
