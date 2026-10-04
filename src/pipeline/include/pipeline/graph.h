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
struct NodePreview {
    size_t values = 0;
    float peak = 0;
};
struct GraphOutput {
    std::vector<float> fft, mel, history, network;
    float peak_hz = 0, magnitude = 0;
    int history_size = 400;
    bool surface = false;
    QMap<int, NodePreview> previews;
};
class GraphProgram final {
  public:
    static QJsonObject defaults(int channel = 0, SpectrumMerge merge = SpectrumMerge::Energy, int source = 0);
    static std::shared_ptr<const GraphProgram> compile(const QJsonObject &document, QString &error);
    bool contains(const QString &kind) const;
    int capture_source() const;
    bool same_processing_as(const GraphProgram &other) const;
    GraphOutput evaluate(const SpectrumFrame &frame, bool previews = true) const;
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
        SurfaceView,
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
