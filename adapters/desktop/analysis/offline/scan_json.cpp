#include "profile_codec.h"
#include "music_archive.h"
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
bool restore_codec_grid(QJsonObject& object,const QJsonObject& placeholder,QString& error) {
    if(object.value("version").toInt()!=3)return true;
    auto metadata=object.value("metadata").toObject();
    if(metadata.value("grid").toObject().value("kind").toString()!="per_track_rfft_hz"){error="Invalid scan frequency grid";return false;}
    auto grid=placeholder.value("metadata").toObject().value("grid").toObject();
    grid["step_hz"]=metadata.value("sample_rate").toDouble()/metadata.value("fft_size").toDouble();
    metadata["grid"]=grid;object["metadata"]=metadata;return true;
}
void restore_codec_payload(QJsonObject& object,const QJsonObject& placeholder,const QJsonObject& scanner) {
    object["training"]=codec_training(placeholder,scanner,object.value("diagnostics").toObject());
    object["gains"]=placeholder.value("gains");object["mean_spectrum"]=placeholder.value("mean_spectrum");
    object["version"]=5;object["kind"]="barelief.frequency_profile";
}
bool restore_codec_fields(QJsonObject& object,QString& error) {
    const auto scanner=object.value("scanner").toObject();
    if(scanner.value("silence_policy").toString()!="exclude_exact_zero_only"){error="Unsupported scan silence policy";return false;}
    ProfileDocument defaults;defaults.profile.compatibility.fft_size=std::size_t(object.value("metadata").toObject().value("fft_size").toInt());
    if(defaults.profile.compatibility.fft_size<512 || defaults.profile.compatibility.fft_size>dsp_kernels::max_fft_samples){error="Invalid scan FFT size";return false;}
    const auto placeholder=QJsonDocument::fromJson(encode_profile(measurement_document(defaults))).object();
    if(!restore_codec_grid(object,placeholder,error))return false;
    restore_codec_payload(object,placeholder,scanner);
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
bool valid_scan_header(const QJsonObject& object,QString& error) {
    const auto version=object.value("version").toDouble();
    if(object.value("kind").toString()!="barelief.music_scan" || (version!=1 && version!=2 && version!=3)){error="Unsupported scan document";return false;}
    return true;
}
bool parse_scan(const QByteArray& bytes,QJsonObject& object,QString& error) {
    if(bytes.size()>limit){error="Scan exceeds 1 GiB limit";return false;}
    QJsonParseError parse;object=QJsonDocument::fromJson(bytes,&parse).object();
    if(parse.error!=QJsonParseError::NoError){error="Malformed scan JSON";return false;}
    return valid_scan_header(object,error);
}
bool save_scan_json(const QString& path,const ProfileDocument& input,QString& error) {
    const auto bytes=encode_music_scan(input);ProfileDocument check;
    if(!decode_music_scan(bytes,check,error))return false;
    QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()){error=file.errorString();return false;}
    return true;
}
}
QJsonObject encode_scan_object(const ProfileDocument& input,ProfileArraySink* sink) {
    auto object=encode_profile_object(input,sink,false);
    const auto grouped=std::ranges::all_of(input.profile.compositions,[](const auto& t){return t.rms_statistics_version==1;});
    object["scanner"]=scanner_settings(object.value("training").toObject(),object.value("scanner").toObject(),grouped);
    for(const auto* key:{"training","gains","mean_spectrum","peak_spectrum","frequency_presence","function"})object.remove(key);
    object["kind"]="barelief.music_scan";object["version"]=3;
    auto metadata=object.value("metadata").toObject(),grid=metadata.value("grid").toObject();
    grid["kind"]="per_track_rfft_hz";grid.remove("step_hz");metadata["grid"]=grid;object["metadata"]=metadata;
    return object;
}
QByteArray encode_music_scan(const ProfileDocument& input){return QJsonDocument(encode_scan_object(input)).toJson(QJsonDocument::Compact);}
bool decode_scan_object(QJsonObject object,ProfileDocument& output,QString& error,ProfileArraySource* source) {
    if(!valid_scan_header(object,error))return false;
    const auto original=object;
    if(!restore_codec_fields(object,error))return false;
    ProfileDocument candidate;
    ProfileReadDiagnostic diagnostic;
    if(!decode_profile_object(object,candidate,error,diagnostic,source))return false;
    if(!complete_measurements(candidate)){error="Scan requires complete per-track measurements";return false;}
    candidate.preserved=original;output=std::move(candidate);return true;
}
bool decode_music_scan(const QByteArray& bytes,ProfileDocument& output,QString& error) {
    QJsonObject object;if(!parse_scan(bytes,object,error))return false;
    return decode_scan_object(object,output,error);
}
bool save_music_scan(const QString& path,const ProfileDocument& input,QString& error) {
    ProfileDocument frontend;frontend.profile.compatibility=input.profile.compatibility;frontend.profile.training=input.profile.training;
    if(!valid_profile_storage(measurement_document(frontend),error) || !valid_scan_measurements(input,error))return false;
    if(!complete_measurements(input)){error="Scan requires complete per-track measurements";return false;}
    if(path.endsWith(".json",Qt::CaseInsensitive))return save_scan_json(path,input,error);
    return save_music_archive(path,input,MusicArchiveKind::Scan,error);
}
bool load_music_scan(const QString& path,ProfileDocument& output,QString& error) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)){error=file.errorString();return false;}
    if(file.peek(4)==QByteArray("PK\x03\x04",4)){ProfileReadDiagnostic diagnostic;return load_music_archive(path,MusicArchiveKind::Scan,output,error,diagnostic);}
    if(file.size()>limit){error="Scan exceeds 1 GiB limit";return false;}
    const auto bytes=file.read(limit+1);
    if(file.error()!=QFileDevice::NoError){error=file.errorString();return false;}
    return decode_music_scan(bytes,output,error);
}
}
