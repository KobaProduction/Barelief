#include "profile_json.h"
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <dsp_kernels/frequency_measurements.h>

namespace desktop_analysis::offline {
namespace {
constexpr qint64 limit=1024ll*1024*1024;
ProfileDocument measurement_document(const ProfileDocument& input) {
    auto result=input;auto& p=result.profile;p.curve.reset();
    p.training.gain_mode=dsp_kernels::ProfileGainMode::MeanEqualization;p.training.min_gain=1;p.training.max_gain=1;
    p.gains.assign(p.compatibility.fft_size/2+1,1);p.mean_spectrum.assign(p.gains.size(),0);
    p.peak_spectrum.clear();p.frequency_presence.clear();return result;
}
QJsonObject scanner_settings(const QJsonObject& training,QJsonObject scanner,bool grouped) {
    for(const auto* key:{"hop","interval_numerator","interval_denominator"})if(!scanner.contains(key))scanner[key]=training.value(key);
    if(grouped)scanner.remove("silence_rms");
    else if(!scanner.contains("silence_rms"))scanner["silence_rms"]=training.value("silence_rms");
    scanner["silence_policy"]="exclude_exact_zero_only";return scanner;
}
QJsonObject codec_training(const QJsonObject& placeholder,const QJsonObject& scanner,const QJsonObject& diagnostics) {
    auto training=placeholder.value("training").toObject();
    for(const auto* key:{"hop","interval_numerator","interval_denominator"})training[key]=scanner.value(key);
    if(scanner.contains("silence_rms"))training["silence_rms"]=scanner.value("silence_rms");
    if(diagnostics.contains("quiet_db"))training["silence_rms"]=std::pow(10.,diagnostics.value("quiet_db").toDouble()/20.);
    return training;
}
bool restore_codec_fields(QJsonObject& object,QString& error) {
    const auto scanner=object.value("scanner").toObject();
    if(scanner.value("silence_policy").toString()!="exclude_exact_zero_only"){error="Unsupported scan silence policy";return false;}
    ProfileDocument defaults;defaults.profile.compatibility.fft_size=std::size_t(object.value("metadata").toObject().value("fft_size").toInt());
    if(defaults.profile.compatibility.fft_size<512 || defaults.profile.compatibility.fft_size>dsp_kernels::max_fft_samples){error="Invalid scan FFT size";return false;}
    const auto placeholder=QJsonDocument::fromJson(encode_profile(measurement_document(defaults))).object();
    object["training"]=codec_training(placeholder,scanner,object.value("diagnostics").toObject());object["gains"]=placeholder.value("gains");object["mean_spectrum"]=placeholder.value("mean_spectrum");
    object["version"]=5;object["kind"]="barelief.frequency_profile";
    return true;
}
bool complete_measurements(const ProfileDocument& document) {
    const auto bins=document.profile.compatibility.fft_size/2+1;
    return !document.profile.compositions.empty() && std::ranges::all_of(document.profile.compositions,[bins](const auto& t){
        return t.mean_spectrum.size()==bins && t.peak_spectrum.size()==bins &&
            ((t.rms_statistics_version==1 && dsp_kernels::valid_rms_bands(t)) ||
             (t.quiet_mean_spectrum.size()==bins && t.quiet_peak_spectrum.size()==bins));
    });
}
bool parse_scan(const QByteArray& bytes,QJsonObject& object,QString& error) {
    if(bytes.size()>limit){error="Scan exceeds 1 GiB limit";return false;}
    QJsonParseError parse;object=QJsonDocument::fromJson(bytes,&parse).object();
    const auto version=object.value("version").toDouble();
    if(parse.error!=QJsonParseError::NoError || object.value("kind").toString()!="barelief.music_scan" || (version!=1 && version!=2)){error="Unsupported or malformed scan document";return false;}
    return true;
}
}
QByteArray encode_music_scan(const ProfileDocument& input) {
    auto object=QJsonDocument::fromJson(encode_profile(measurement_document(input))).object();
    const auto grouped=std::ranges::all_of(input.profile.compositions,[](const auto& t){return t.rms_statistics_version==1;});
    object["scanner"]=scanner_settings(object.value("training").toObject(),object.value("scanner").toObject(),grouped);
    for(const auto* key:{"training","gains","mean_spectrum","peak_spectrum","frequency_presence","function"})object.remove(key);
    object["kind"]="barelief.music_scan";object["version"]=2;
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
bool decode_music_scan(const QByteArray& bytes,ProfileDocument& output,QString& error) {
    QJsonObject object;if(!parse_scan(bytes,object,error))return false;
    const auto original=object;
    if(!restore_codec_fields(object,error))return false;
    ProfileDocument candidate;
    if(!decode_profile(QJsonDocument(object).toJson(QJsonDocument::Compact),candidate,error))return false;
    if(!complete_measurements(candidate)){error="Scan requires complete per-track measurements";return false;}
    candidate.preserved=original;output=std::move(candidate);return true;
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
