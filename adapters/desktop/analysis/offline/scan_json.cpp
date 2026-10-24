#include "profile_json.h"
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>

namespace desktop_analysis::offline {
namespace {
constexpr qint64 limit=1024ll*1024*1024;
ProfileDocument measurement_document(const ProfileDocument& input) {
    auto result=input;auto& p=result.profile;p.curve.reset();
    p.training.gain_mode=dsp_kernels::ProfileGainMode::MeanEqualization;p.training.min_gain=1;p.training.max_gain=1;
    p.gains.assign(p.compatibility.fft_size/2+1,1);p.mean_spectrum.assign(p.gains.size(),0);
    p.peak_spectrum.clear();p.frequency_presence.clear();return result;
}
QJsonObject scanner_settings(const QJsonObject& training,QJsonObject scanner) {
    for(const auto* key:{"silence_rms","hop","interval_numerator","interval_denominator"})if(!scanner.contains(key))scanner[key]=training.value(key);
    scanner["silence_policy"]="exclude_exact_zero_only";return scanner;
}
QJsonObject codec_training(const QJsonObject& placeholder,const QJsonObject& scanner) {
    auto training=placeholder.value("training").toObject();
    for(const auto* key:{"silence_rms","hop","interval_numerator","interval_denominator"})training[key]=scanner.value(key);
    return training;
}
bool restore_codec_fields(QJsonObject& object,QString& error) {
    const auto scanner=object.value("scanner").toObject();
    if(scanner.value("silence_policy").toString()!="exclude_exact_zero_only"){error="Unsupported scan silence policy";return false;}
    ProfileDocument defaults;defaults.profile.compatibility.fft_size=std::size_t(object.value("metadata").toObject().value("fft_size").toInt());
    if(defaults.profile.compatibility.fft_size<512 || defaults.profile.compatibility.fft_size>dsp_kernels::max_fft_samples){error="Invalid scan FFT size";return false;}
    const auto placeholder=QJsonDocument::fromJson(encode_profile(measurement_document(defaults))).object();
    object["training"]=codec_training(placeholder,scanner);object["gains"]=placeholder.value("gains");object["mean_spectrum"]=placeholder.value("mean_spectrum");object["version"]=3;
    return true;
}
bool complete_measurements(const ProfileDocument& document) {
    const auto bins=document.profile.compatibility.fft_size/2+1;
    return !document.profile.compositions.empty() && std::ranges::all_of(document.profile.compositions,[bins](const auto& t){
        return t.mean_spectrum.size()==bins && t.peak_spectrum.size()==bins &&
            t.quiet_mean_spectrum.size()==bins && t.quiet_peak_spectrum.size()==bins;
    });
}
bool parse_scan(const QByteArray& bytes,QJsonObject& object,QString& error) {
    if(bytes.size()>limit){error="Scan exceeds 1 GiB limit";return false;}
    QJsonParseError parse;object=QJsonDocument::fromJson(bytes,&parse).object();
    if(parse.error!=QJsonParseError::NoError || object.value("kind").toString()!="barelief.music_scan" || object.value("version").toDouble()!=1){error="Unsupported or malformed scan document";return false;}
    return true;
}
}
QByteArray encode_music_scan(const ProfileDocument& input) {
    auto object=QJsonDocument::fromJson(encode_profile(measurement_document(input))).object();
    object["scanner"]=scanner_settings(object.value("training").toObject(),object.value("scanner").toObject());
    for(const auto* key:{"training","gains","mean_spectrum","peak_spectrum","frequency_presence","function"})object.remove(key);
    object["kind"]="barelief.music_scan";object["version"]=1;
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
bool decode_music_scan(const QByteArray& bytes,ProfileDocument& output,QString& error) {
    QJsonObject object;if(!parse_scan(bytes,object,error) || !restore_codec_fields(object,error))return false;
    ProfileDocument candidate;
    if(!decode_profile(QJsonDocument(object).toJson(QJsonDocument::Compact),candidate,error))return false;
    if(!complete_measurements(candidate)){error="Scan requires complete per-track measurements";return false;}
    output=std::move(candidate);return true;
}
bool save_music_scan(const QString& path,const ProfileDocument& input,QString& error) {
    const auto bytes=encode_music_scan(input);ProfileDocument check;
    if(!decode_music_scan(bytes,check,error))return false;
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()){error=file.errorString();return false;}
    return true;
}
bool load_music_scan(const QString& path,ProfileDocument& output,QString& error) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)){error=file.errorString();return false;}
    if(file.size()>limit){error="Scan exceeds 1 GiB limit";return false;}
    const auto bytes=file.read(limit+1);
    if(file.error()!=QFileDevice::NoError){error=file.errorString();return false;}
    return decode_music_scan(bytes,output,error);
}
}
