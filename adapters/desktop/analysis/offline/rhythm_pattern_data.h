#pragma once
#include <dsp_kernels/pattern_state_decoder.h>
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>

namespace desktop_analysis::offline {
struct RhythmPatternDataLimits {
    std::size_t maximum_bytes=64*1024*1024,maximum_patterns=64,maximum_gmms=4096;
    std::size_t maximum_features=256,maximum_components=1024,maximum_values=8*1024*1024;
};
enum class RhythmPatternDataError {None,Encoding,Version,Format,Limit,Identifier,Model};
struct RhythmPatternDataDiagnostic {RhythmPatternDataError reason=RhythmPatternDataError::None;QString field;};
// Cold JSON adapter, never used by prepared processing. Ordered models and
// stable pattern/GMM IDs. Unknown fields remain in preserved and survive encode.
struct RhythmPatternDataDocument {
    std::vector<dsp_kernels::GmmPatternData> patterns;
    QStringList ids;
    std::vector<QStringList> gmm_ids;
    QJsonObject preserved;
};
bool decode_rhythm_pattern_data(const QByteArray&,RhythmPatternDataDocument&,
                                RhythmPatternDataDiagnostic&,RhythmPatternDataLimits={});
QByteArray encode_rhythm_pattern_data(const RhythmPatternDataDocument&);
bool load_rhythm_pattern_data(const QString& path,RhythmPatternDataDocument&,
                              RhythmPatternDataDiagnostic&,RhythmPatternDataLimits={});
}
