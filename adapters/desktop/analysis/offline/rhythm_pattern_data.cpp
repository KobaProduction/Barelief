#include "rhythm_pattern_data.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace desktop_analysis::offline {
namespace {
using namespace dsp_kernels;
struct Budget {RhythmPatternDataLimits limits;std::size_t values=0,gmms=0;};
bool fail(RhythmPatternDataDiagnostic& error,RhythmPatternDataError reason,const char* field) {
    error={reason,QString::fromLatin1(field)};return false;
}
bool count(const QJsonObject& object,const char* key,std::size_t maximum,std::size_t& output) {
    const auto value=object.value(QLatin1String(key));
    if(!value.isDouble())return false;
    const double number=value.toDouble();
    if(!std::isfinite(number) || number<1 || std::floor(number)!=number || number>double(maximum))return false;
    output=std::size_t(number);return true;
}
bool identifier(const QJsonObject& object,QString& id) {
    const auto value=object.value(QLatin1String("id"));
    if(!value.isString() || value.toString().isEmpty())return false;
    id=value.toString();return true;
}
bool covariance(const QString& value,GaussianCovariance& output) {
    if(value==QLatin1String("diag"))output=GaussianCovariance::Diagonal;
    else if(value==QLatin1String("spherical"))output=GaussianCovariance::Spherical;
    else if(value==QLatin1String("tied"))output=GaussianCovariance::Tied;
    else if(value==QLatin1String("full"))output=GaussianCovariance::Full;
    else return false;
    return true;
}
QString covariance_name(GaussianCovariance value) {
    switch(value){
    case GaussianCovariance::Diagonal:return QStringLiteral("diag");
    case GaussianCovariance::Spherical:return QStringLiteral("spherical");
    case GaussianCovariance::Tied:return QStringLiteral("tied");
    case GaussianCovariance::Full:return QStringLiteral("full");
    }
    return {};
}
bool append_number(const QJsonValue& value,std::vector<float>& output) {
    if(!value.isDouble() || !std::isfinite(value.toDouble()))return false;
    const float number=float(value.toDouble());
    if(!std::isfinite(number))return false;
    output.push_back(number);return true;
}

bool values(const QJsonObject& object,const char* key,std::vector<float>& output,Budget& budget) {
    const auto value=object.value(QLatin1String(key));
    if(!value.isArray())return false;
    const auto array=value.toArray();
    if(std::size_t(array.size())>budget.limits.maximum_values-budget.values)return false;
    output.reserve(std::size_t(array.size()));
    for(const auto& item:array)if(!append_number(item,output))return false;
    budget.values+=output.size();return true;
}
bool mixture(const QJsonObject& object,GaussianMixtureData& output,Budget& budget,RhythmPatternDataDiagnostic& error) {
    if(budget.gmms==budget.limits.maximum_gmms)return fail(error,RhythmPatternDataError::Limit,"gmms");
    GaussianMixtureData data;
    if(!count(object,"features",budget.limits.maximum_features,data.features) ||
       !count(object,"components",budget.limits.maximum_components,data.components) ||
       !covariance(object.value(QLatin1String("covariance_type")).toString(),data.covariance))
        return fail(error,RhythmPatternDataError::Format,"gmm.shape");
    if(!values(object,"weights",data.weights,budget) || !values(object,"means",data.means,budget) ||
       !values(object,"covariances",data.covariances,budget))return fail(error,RhythmPatternDataError::Format,"gmm.values");
    PreparedGaussianMixture admitted;
    if(!admitted.prepare(data.parameters()))return fail(error,RhythmPatternDataError::Model,"gmm");
    ++budget.gmms;output=std::move(data);return true;
}
bool pattern_shape(const QJsonObject& object,const RhythmPatternDataDocument& output,QString& id,std::size_t& beats,RhythmPatternDataDiagnostic& error) {
    if(!identifier(object,id) || output.ids.contains(id))return fail(error,RhythmPatternDataError::Identifier,"pattern.id");
    if(!count(object,"num_beats",UINT32_MAX,beats) || !object.value(QLatin1String("gmms")).isArray())
        return fail(error,RhythmPatternDataError::Format,"pattern");
    return true;
}
bool append_mixture(const QJsonValue& value,GmmPatternData& data,QStringList& ids,Budget& budget,RhythmPatternDataDiagnostic& error) {
    if(!value.isObject())return fail(error,RhythmPatternDataError::Format,"gmm");
    const auto object=value.toObject();QString id;
    if(!identifier(object,id) || ids.contains(id))return fail(error,RhythmPatternDataError::Identifier,"gmm.id");
    GaussianMixtureData gmm;
    if(!mixture(object,gmm,budget,error))return false;
    data.gmms.push_back(std::move(gmm));ids.append(id);return true;
}
bool pattern_mixtures(const QJsonArray& gmms,GmmPatternData& data,QStringList& ids,Budget& budget,RhythmPatternDataDiagnostic& error) {
    if(gmms.isEmpty())return fail(error,RhythmPatternDataError::Format,"pattern.gmms");
    for(const auto& value:gmms)if(!append_mixture(value,data,ids,budget,error))return false;
    return true;
}

bool pattern(const QJsonObject& object,RhythmPatternDataDocument& output,Budget& budget,RhythmPatternDataDiagnostic& error) {
    QString id;std::size_t beats=0;
    if(!pattern_shape(object,output,id,beats,error))return false;
    const auto gmms=object.value(QLatin1String("gmms")).toArray();
    GmmPatternData data;data.beats=std::uint32_t(beats);QStringList ids;
    if(!pattern_mixtures(gmms,data,ids,budget,error))return false;
    output.patterns.push_back(std::move(data));output.ids.append(id);output.gmm_ids.push_back(std::move(ids));return true;
}
bool root_format(const QJsonObject& root,RhythmPatternDataDiagnostic& error) {
    if(root.value(QLatin1String("version"))!=QJsonValue(1))return fail(error,RhythmPatternDataError::Version,"version");
    if(root.value(QLatin1String("format"))!=QJsonValue(QStringLiteral("barelief.rhythm-patterns")) ||
       root.value(QLatin1String("dtype"))!=QJsonValue(QStringLiteral("float32")) ||
       !root.value(QLatin1String("patterns")).isArray())return fail(error,RhythmPatternDataError::Format,"format");
    return true;
}
bool parse_root(const QByteArray& bytes,RhythmPatternDataLimits limits,QJsonObject& root,RhythmPatternDataDiagnostic& error) {
    if(std::size_t(bytes.size())>limits.maximum_bytes)return fail(error,RhythmPatternDataError::Limit,"bytes");
    QJsonParseError parse;const auto document=QJsonDocument::fromJson(bytes,&parse);
    if(parse.error!=QJsonParseError::NoError || !document.isObject())return fail(error,RhythmPatternDataError::Encoding,"json");
    root=document.object();return root_format(root,error);
}
bool document_patterns(const QJsonObject& root,RhythmPatternDataDocument& output,Budget& budget,RhythmPatternDataDiagnostic& error) {
    const auto patterns=root.value(QLatin1String("patterns")).toArray();
    if(patterns.isEmpty() || std::size_t(patterns.size())>budget.limits.maximum_patterns)
        return fail(error,RhythmPatternDataError::Limit,"patterns");
    for(const auto& item:patterns){
        if(!item.isObject())return fail(error,RhythmPatternDataError::Format,"pattern");
        if(!pattern(item.toObject(),output,budget,error))return false;
    }
    return true;
}

QJsonObject by_id(const QJsonArray& array,const QString& id) {
    for(const auto& item:array)if(item.toObject().value(QLatin1String("id")).toString()==id)return item.toObject();
    return {};
}
QJsonArray numeric(std::span<const float> values) {
    QJsonArray result;
    for(const float value:values)result.append(double(value));
    return result;
}
QJsonObject encode_mixture(const GaussianMixtureData& data,const QString& id,QJsonObject object) {
    object[QStringLiteral("id")]=id;object[QStringLiteral("features")]=double(data.features);
    object[QStringLiteral("components")]=double(data.components);object[QStringLiteral("covariance_type")]=covariance_name(data.covariance);
    object[QStringLiteral("weights")]=numeric(data.weights);object[QStringLiteral("means")]=numeric(data.means);
    object[QStringLiteral("covariances")]=numeric(data.covariances);return object;
}
QJsonObject encode_pattern(const GmmPatternData& data,const QString& id,const QStringList& ids,QJsonObject object) {
    const auto previous=object.value(QLatin1String("gmms")).toArray();QJsonArray gmms;
    for(std::size_t i=0;i<data.gmms.size();++i)
        gmms.append(encode_mixture(data.gmms[i],ids[qsizetype(i)],by_id(previous,ids[qsizetype(i)])));
    object[QStringLiteral("id")]=id;object[QStringLiteral("num_beats")]=double(data.beats);
    object[QStringLiteral("gmms")]=gmms;return object;
}
}
bool decode_rhythm_pattern_data(const QByteArray& bytes,RhythmPatternDataDocument& output,
                                RhythmPatternDataDiagnostic& error,RhythmPatternDataLimits limits) {
    error={};QJsonObject root;
    if(!parse_root(bytes,limits,root,error))return false;
    RhythmPatternDataDocument next;next.preserved=root;Budget budget{limits};
    if(!document_patterns(root,next,budget,error))return false;
    output=std::move(next);return true;
}
QByteArray encode_rhythm_pattern_data(const RhythmPatternDataDocument& data) {
    if(data.patterns.size()!=std::size_t(data.ids.size()) || data.gmm_ids.size()!=data.patterns.size())return {};
    auto root=data.preserved;const auto previous=root.value(QLatin1String("patterns")).toArray();QJsonArray patterns;
    for(std::size_t i=0;i<data.patterns.size();++i){
        if(data.patterns[i].gmms.size()!=std::size_t(data.gmm_ids[i].size()))return {};
        const auto id=data.ids[qsizetype(i)];
        patterns.append(encode_pattern(data.patterns[i],id,data.gmm_ids[i],by_id(previous,id)));
    }
    root[QStringLiteral("format")]=QStringLiteral("barelief.rhythm-patterns");root[QStringLiteral("dtype")]=QStringLiteral("float32");
    root[QStringLiteral("version")]=1;root[QStringLiteral("patterns")]=patterns;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
bool load_rhythm_pattern_data(const QString& path,RhythmPatternDataDocument& output,
                              RhythmPatternDataDiagnostic& error,RhythmPatternDataLimits limits) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly))return fail(error,RhythmPatternDataError::Encoding,"file");
    if(file.size()<0 || std::uint64_t(file.size())>limits.maximum_bytes)return fail(error,RhythmPatternDataError::Limit,"bytes");
    return decode_rhythm_pattern_data(file.readAll(),output,error,limits);
}
}
