#include <offline/rhythm_pattern_data.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <iostream>
#include <stdexcept>

namespace {
using namespace desktop_analysis::offline;
void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
QJsonObject fixture() {
    return QJsonDocument::fromJson(R"json({
        "version":1,"format":"barelief.rhythm-patterns","dtype":"float32",
        "extension":{"number":42},
        "patterns":[{"id":"pattern","num_beats":4,"origin":"pattern-extension",
            "gmms":[{"id":"component","features":1,"components":1,
                "covariance_type":"diag","weights":[1],"means":[0],
                "covariances":[1],"extension":"gmm-extension"}]}]
    })json").object();
}
QByteArray bytes(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
QJsonObject with_gmm_field(const char* key,const QJsonValue& value) {
    auto root=fixture();
    auto patterns=root.value(QLatin1String("patterns")).toArray();
    auto pattern=patterns[0].toObject();
    auto gmms=pattern.value(QLatin1String("gmms")).toArray();
    auto mixture=gmms[0].toObject();
    mixture[QString::fromLatin1(key)]=value;
    gmms[0]=mixture;pattern[QStringLiteral("gmms")]=gmms;
    patterns[0]=pattern;root[QStringLiteral("patterns")]=patterns;
    return root;
}
void rejects(const QByteArray& candidate,RhythmPatternDataError expected,const char* field,RhythmPatternDataLimits limits={}) {
    RhythmPatternDataDocument output;RhythmPatternDataDiagnostic error;
    require(decode_rhythm_pattern_data(bytes(fixture()),output,error),"valid baseline");
    const auto before=encode_rhythm_pattern_data(output);
    require(!decode_rhythm_pattern_data(candidate,output,error,limits),"invalid document must be rejected");
    require(error.reason==expected,"rejection reason");
    require(error.field==QLatin1String(field),"rejection field");
    require(encode_rhythm_pattern_data(output)==before,"rejected input must preserve the loaded document");
}
void roundtrip_extensions() {
    const auto original=fixture();RhythmPatternDataDocument output;
    RhythmPatternDataDiagnostic error{RhythmPatternDataError::Format,QStringLiteral("old")};
    require(decode_rhythm_pattern_data(bytes(original),output,error),"valid document");
    require(error.reason==RhythmPatternDataError::None && error.field.isEmpty(),"successful decode clears diagnostics");
    require(output.ids==QStringList{QStringLiteral("pattern")},"stable pattern identity");
    require(output.patterns.size()==1 && output.patterns[0].beats==4,"pattern shape");
    require(output.gmm_ids.size()==1 && output.gmm_ids[0]==QStringList{QStringLiteral("component")},"stable GMM identity");
    require(QJsonDocument::fromJson(encode_rhythm_pattern_data(output)).object()==original,"roundtrip preserves all extension fields");
}
void malformed_documents() {
    rejects(QByteArray("{"),RhythmPatternDataError::Encoding,"json");
    auto future=fixture();future[QStringLiteral("version")]=2;
    rejects(bytes(future),RhythmPatternDataError::Version,"version");
    auto dtype=fixture();dtype[QStringLiteral("dtype")]=QStringLiteral("float64");
    rejects(bytes(dtype),RhythmPatternDataError::Format,"format");
    auto invalid=fixture();invalid[QStringLiteral("patterns")]=QJsonArray{7};
    rejects(bytes(invalid),RhythmPatternDataError::Format,"pattern");
    auto empty=fixture();auto pattern=empty.value(QLatin1String("patterns")).toArray()[0].toObject();
    pattern[QStringLiteral("gmms")]=QJsonArray{};empty[QStringLiteral("patterns")]=QJsonArray{pattern};
    rejects(bytes(empty),RhythmPatternDataError::Format,"pattern.gmms");
}
void invalid_numbers() {
    rejects(bytes(with_gmm_field("means",QJsonArray{QStringLiteral("bad")})),RhythmPatternDataError::Format,"gmm.values");
    rejects(bytes(with_gmm_field("means",QJsonArray{1e100})),RhythmPatternDataError::Format,"gmm.values");
    rejects(bytes(with_gmm_field("covariances",QJsonArray{0.})),RhythmPatternDataError::Model,"gmm");
}
void duplicate_identities() {
    auto root=fixture();auto patterns=root.value(QLatin1String("patterns")).toArray();
    patterns.append(patterns[0]);root[QStringLiteral("patterns")]=patterns;
    rejects(bytes(root),RhythmPatternDataError::Identifier,"pattern.id");
    root=fixture();patterns=root.value(QLatin1String("patterns")).toArray();
    auto pattern=patterns[0].toObject();auto gmms=pattern.value(QLatin1String("gmms")).toArray();
    gmms.append(gmms[0]);pattern[QStringLiteral("gmms")]=gmms;
    patterns[0]=pattern;root[QStringLiteral("patterns")]=patterns;
    rejects(bytes(root),RhythmPatternDataError::Identifier,"gmm.id");
}
void resource_limits() {
    const auto valid=bytes(fixture());RhythmPatternDataLimits limits;
    limits.maximum_bytes=1;rejects(valid,RhythmPatternDataError::Limit,"bytes",limits);
    limits={};limits.maximum_patterns=0;rejects(valid,RhythmPatternDataError::Limit,"patterns",limits);
    limits={};limits.maximum_gmms=0;rejects(valid,RhythmPatternDataError::Limit,"gmms",limits);
    limits={};limits.maximum_values=2;rejects(valid,RhythmPatternDataError::Format,"gmm.values",limits);
}
}
int main() {
    try {
        roundtrip_extensions();malformed_documents();invalid_numbers();duplicate_identities();resource_limits();
        std::cout<<"Rhythm pattern codec checks passed\n";return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
