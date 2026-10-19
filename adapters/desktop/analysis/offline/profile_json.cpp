#include "profile_json.h"
#include <signal_contracts/spectral_calibration.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtZlib/zlib.h>
#include <cmath>
#include <bit>
#include <type_traits>
#include <algorithm>
#include <utility>

namespace desktop_analysis::offline {
namespace {
constexpr qsizetype max_json_bytes=1024ll*1024*1024;
using namespace dsp_kernels;
QString window_name(WindowFunction window){return QString::fromUtf8(window_function_name(window));}
QString spectrum_name(SpectrumProjection s) {return QString::fromUtf8(spectrum_projection_name(s));}
QString resampler_name(ResamplerMode mode) {
    return QString::fromUtf8(signal_contracts::resampler_mode_name(mode));
}
bool resampler_mode(const QString& text,ResamplerMode& mode) {
    return signal_contracts::parse_resampler_mode(text.toStdString(),mode);
}
bool number(const QJsonObject& o,const char* key,double& v) {auto x=o.value(key);if(!x.isDouble())return false;v=x.toDouble();return std::isfinite(v);}
bool count(const QJsonObject& o,const char* key,std::size_t& v) {double n;if(!number(o,key,n)||n<0||n>1000000000000.0||std::floor(n)!=n)return false;v=std::size_t(n);return true;}
template<class T> QJsonObject array(const std::vector<T>& values,const QJsonValue& previous={}) {
    using Bits=std::conditional_t<sizeof(T)==4,std::uint32_t,std::uint64_t>;
    QByteArray raw;raw.resize(qsizetype(values.size()*sizeof(T)));
    for(std::size_t i=0;i<values.size();++i){const auto bits=std::bit_cast<Bits>(values[i]);
        for(std::size_t j=0;j<sizeof(T);++j)raw[qsizetype(i*sizeof(T)+j)]=char(bits>>(8*j));}
    auto result=previous.toObject();result["encoding"]=sizeof(T)==4?"f32le-qcompress-base64":"f64le-qcompress-base64";result["count"]=double(values.size());result["data"]=QString::fromLatin1(qCompress(raw,3).toBase64());return result;
}
template<class T> bool decode_numeric_array(const QJsonArray& source,std::vector<T>& decoded,std::size_t count) {
    if(std::size_t(source.size())!=count)return false;
    for(const auto& item:source){
        if(!item.isDouble() || !std::isfinite(item.toDouble()) || item.toDouble()<0)return false;
        const auto value=T(item.toDouble());if(!std::isfinite(value))return false;
        decoded.push_back(value);
    }
    return true;
}
template<class T> bool packed_array(const QJsonObject& object,std::size_t expected,QByteArray& packed) {
    std::size_t elements=0;
    if(object.value("encoding").toString()!=(sizeof(T)==4?"f32le-qcompress-base64":"f64le-qcompress-base64") || !count(object,"count",elements) || elements!=expected || !object.value("data").isString())return false;
    const auto text=object.value("data").toString().toLatin1();
    const auto conversion=QByteArray::fromBase64Encoding(text,QByteArray::AbortOnBase64DecodingErrors);
    if(!conversion || conversion.decoded.toBase64()!=text || conversion.decoded.size()<4)return false;
    packed=conversion.decoded;return true;
}
template<class T> bool unpack_array(const QJsonObject& object,std::size_t expected,QByteArray& raw) {
    QByteArray packed;if(!packed_array<T>(object,expected,packed))return false;
    std::uint32_t length=0;
    for(int j=0;j<4;++j)length=(length<<8)|static_cast<unsigned char>(packed[j]);
    if(length!=expected*sizeof(T))return false;
    raw.resize(qsizetype(length));uLongf output_size=length;uLong input_size=uLong(packed.size()-4);
    return uncompress2(reinterpret_cast<Bytef*>(raw.data()),&output_size,reinterpret_cast<const Bytef*>(packed.constData()+4),&input_size)==Z_OK && output_size==length && input_size==uLong(packed.size()-4);
}
template<class T> bool decode_binary_array(const QByteArray& raw,std::vector<T>& decoded,std::size_t count) {
    using Bits=std::conditional_t<sizeof(T)==4,std::uint32_t,std::uint64_t>;
    for(std::size_t i=0;i<count;++i){
        Bits bits=0;
        for(std::size_t j=0;j<sizeof(T);++j)bits|=Bits(static_cast<unsigned char>(raw[qsizetype(i*sizeof(T)+j)]))<<(8*j);
        const auto value=std::bit_cast<T>(bits);if(!std::isfinite(value) || value<0)return false;
        decoded.push_back(value);
    }
    return true;
}
template<class T> bool vector(const QJsonValue& value,std::vector<T>& result,std::size_t count,std::size_t& budget) {
    if(count>budget/sizeof(T))return false;
    budget-=count*sizeof(T);std::vector<T> decoded;decoded.reserve(count);
    if(value.isArray()){
        if(!decode_numeric_array(value.toArray(),decoded,count))return false;
    }else if(value.isObject()){
        QByteArray raw;
        if(!unpack_array<T>(value.toObject(),count,raw) || !decode_binary_array(raw,decoded,count))return false;
    }else return false;
    result=std::move(decoded);return true;
}
bool valid_axis(const std::vector<double>& coordinates,std::size_t size,double maximum) {
    if(coordinates.size()!=size)return false;
    double previous=-1;
    for(double coordinate:coordinates){
        if(!std::isfinite(coordinate) || coordinate<0 || coordinate<=previous || coordinate>maximum)return false;
        previous=coordinate;
    }
    return true;
}
bool valid_preview_coordinates(const SpectrogramPreview& image) {
    return valid_axis(image.time_seconds,image.rows,image.duration_seconds) &&
        valid_axis(image.frequency_hz,image.bands,image.sample_rate/2);
}
template<class T> bool valid_values(const std::vector<T>& values) {
    return std::all_of(values.begin(),values.end(),[](auto value){return std::isfinite(value) && value>=0;});
}
bool valid_composition(const CompositionStatistics& track,std::size_t bins) {
    if(track.active_frames>track.frames || !std::isfinite(track.rms) || track.rms<0 || (!track.mean_spectrum.empty() && track.mean_spectrum.size()!=bins) || (!track.frequency_presence.empty() && track.frequency_presence.size()!=bins))return false;
    return valid_values(track.mean_spectrum) && valid_values(track.frequency_presence) &&
        std::none_of(track.frequency_presence.begin(),track.frequency_presence.end(),[](double value){return value>1;});
}
bool valid_preview(const SpectrogramPreview& image,const CompositionStatistics& track,const ProfileCompatibility& frontend,std::size_t bins) {
    if(image.rows>2048 || image.bands<1 || image.bands>2048 || image.bands>bins || image.values.size()!=image.rows*image.bands)return false;
    if(image.source!=QString::fromStdString(track.source) || image.sample_rate!=frontend.sample_rate || image.bin_hz!=frontend.sample_rate/frontend.fft_size || image.duration_seconds!=double(track.samples)/frontend.sample_rate)return false;
    return valid_values(image.values) && valid_preview_coordinates(image);
}
bool validate_saved_composition(const ProfileDocument& document,std::size_t index,std::size_t& bytes,QString& error) {
    const auto& profile=document.profile;const auto& track=profile.compositions[index];
    if(!valid_composition(track,profile.gains.size())){error="Invalid saved composition statistics";return false;}
    bytes+=(track.mean_spectrum.size()+track.frequency_presence.size())*sizeof(double);
    if(index<document.previews.size() && document.previews[index].rows){
        const auto& image=document.previews[index];
        if(!valid_preview(image,track,profile.compatibility,profile.gains.size())){error="Invalid saved composition preview";return false;}
        bytes+=image.values.size()*sizeof(float)+(image.time_seconds.size()+image.frequency_hz.size())*sizeof(double);
    }
    if(bytes>1024ull*1024*1024){error="Decoded profile exceeds 1 GiB limit";return false;}
    return true;
}
bool valid_document(const ProfileDocument& document,QString& error) {
    const auto& profile=document.profile;std::string validation;
    if(!validate_profile(profile,profile.compatibility,validation)){error=QString::fromStdString(validation);return false;}
    if(profile.compositions.size()>4096 || (!document.previews.empty() && document.previews.size()!=profile.compositions.size())){error="Invalid saved composition count";return false;}
    std::size_t bytes=profile.gains.size()*sizeof(float)+(profile.mean_spectrum.size()+profile.frequency_presence.size())*sizeof(double);
    for(std::size_t i=0;i<profile.compositions.size();++i)if(!validate_saved_composition(document,i,bytes,error))return false;
    return true;
}
}
namespace {
class ProfileDecoder {
    const QJsonObject& source_;
    ProfileDocument candidate_;
    QString& error_;
    std::size_t budget_=1024ull*1024*1024;
    ProfileReadDiagnostic& diagnostic_;

    ProfileCompatibility& frontend(){return candidate_.profile.compatibility;}
    bool fail(const char* text,ProfileReadIssue reason=ProfileReadIssue::Encoding,const char* parameter="profile") {
        error_=QString::fromUtf8(text);diagnostic_={reason,QString::fromUtf8(parameter)};return false;
    }
    bool resampler(const QJsonObject& metadata){
        const auto object=metadata.value("resampler").toObject();std::size_t quality=0,design=0;
        auto& c=frontend();
        if(!resampler_mode(object.value("mode").toString(),c.resampler_mode) || !count(object,"quality",quality) || quality>10 || !count(object,"design_version",design) || design!=PreparedResampler::design_version)return fail("Missing or unsupported resampler provenance",ProfileReadIssue::Resampler,"resampler");
        c.resampler_quality=unsigned(quality);c.resampler_design_version=std::uint32_t(design);return true;
    }
    bool channels(const QJsonObject& metadata){
        const auto object=metadata.value("channels").toObject();std::size_t selected=0;
        if(!count(object,"index",selected) || selected>255)return fail("Invalid channel index",ProfileReadIssue::Frontend,"channel_index");
        frontend().channel_index=std::uint32_t(selected);
        const auto policy=object.value("policy").toString();
        if(policy=="mean")frontend().channel_policy=ChannelPolicy::Mean;
        else if(policy=="select")frontend().channel_policy=ChannelPolicy::Select;
        else return fail("Invalid channel policy",ProfileReadIssue::Frontend,"channel_policy");
        return true;
    }
    bool projection(const QJsonObject& metadata){
        auto& c=frontend();
        if(!parse_window_function(metadata.value("window").toString().toStdString(),c.window))return fail("Unknown window",ProfileReadIssue::Window,"window_function");
        if(!parse_spectrum_projection(metadata.value("spectrum").toString().toStdString(),c.projection.representation) || !is_amplitude_spectrum_projection(c.projection.representation))return fail("Unsupported spectrum",ProfileReadIssue::Representation,"representation");
        const auto normalization=metadata.value("normalization").toString();
        if(normalization=="one_sided_window_corrected_v1")c.projection.compensate=true;
        else if(normalization=="raw_rfft_v1")c.projection.compensate=false;
        else return fail("Unsupported spectrum compensation",ProfileReadIssue::Representation,"normalize");
        return true;
    }
    bool metadata(){
        const auto object=source_.value("metadata").toObject();auto& c=frontend();
        if(!number(object,"sample_rate",c.sample_rate) || c.sample_rate<=0)return fail("Invalid sample rate",ProfileReadIssue::SampleRate,"sample_rate");
        if(!count(object,"fft_size",c.fft_size) || c.fft_size<2 || (c.fft_size&(c.fft_size-1)))return fail("Invalid FFT size",ProfileReadIssue::FftSize,"fft_size");
        if(!number(object,"reference",c.reference) || !number(object,"floor_db",c.floor_db))return fail("Missing profile metadata",ProfileReadIssue::Frontend,"reference");
        if(!number(object,"coherent_gain",c.coherent_gain) || c.coherent_gain<0)return fail("Invalid coherent gain",ProfileReadIssue::CoherentGain,"coherent_gain");
        if(!number(object,"power_gain",c.power_gain) || c.power_gain<0)return fail("Invalid power gain",ProfileReadIssue::PowerGain,"power_gain");
        if(!object.value("include_dc").isBool() || !object.value("include_nyquist").isBool())return fail("Missing explicit spectral layout",ProfileReadIssue::Layout,"include_dc");
        c.include_dc=object.value("include_dc").toBool();c.include_nyquist=object.value("include_nyquist").toBool();
        return resampler(object) && channels(object) && projection(object);
    }
    bool training(){
        const auto object=source_.value("training").toObject();auto& t=candidate_.profile.training;
        if(!number(object,"silence_rms",t.silence_rms) || !number(object,"min_gain",t.min_gain) || !number(object,"max_gain",t.max_gain) || !count(object,"smoothing_radius",t.smoothing_radius) || !count(object,"hop",t.hop) || object.value("weighting").toString()!="equal_active_composition")return fail("Invalid training settings");
        std::size_t numerator=0,denominator=0;
        if(!count(object,"interval_numerator",numerator) || !count(object,"interval_denominator",denominator))return fail("Missing analysis cadence");
        t.interval_numerator=numerator;t.interval_denominator=denominator;
        std::string validation;
        if(valid_profile_settings(frontend(),t,validation))return true;
        error_=QString::fromStdString(validation);diagnostic_={ProfileReadIssue::Frontend,QStringLiteral("training")};return false;
    }
    template<class T> bool binary_coefficients(const QJsonObject& value,const char* key,std::vector<T>& output,std::size_t bins) {
        QByteArray raw;
        if(bins>budget_/sizeof(T) || !unpack_array<T>(value,bins,raw))return fail("Invalid coefficient encoding",ProfileReadIssue::Encoding,key);
        std::vector<T> decoded;decoded.reserve(bins);
        if(!decode_binary_array(raw,decoded,bins))return fail("Invalid coefficient value",ProfileReadIssue::GainValue,key);
        budget_-=bins*sizeof(T);output=std::move(decoded);return true;
    }

    template<class T> bool coefficient_vector(const char* key,std::vector<T>& output,std::size_t bins) {
        const auto value=source_.value(QString::fromUtf8(key));
        if(value.isArray() && std::size_t(value.toArray().size())!=bins)return fail("Wrong coefficient count",ProfileReadIssue::GainCount,key);
        if(value.isObject()){
            std::size_t declared=0;
            if(!count(value.toObject(),"count",declared))return fail("Invalid coefficient encoding",ProfileReadIssue::Encoding,key);
            if(declared!=bins)return fail("Wrong coefficient count",ProfileReadIssue::GainCount,key);
            if(!binary_coefficients(value.toObject(),key,output,bins))return false;
        }
        else if(value.isArray()){
            if(!vector(value,output,bins,budget_))return fail("Invalid profile coefficients",ProfileReadIssue::GainValue,key);
        }else return fail("Invalid coefficient encoding",ProfileReadIssue::Encoding,key);
        if constexpr(std::is_same_v<T,float>){
            if(!std::all_of(output.begin(),output.end(),[](float value){return signal_contracts::valid_profile_gain(value);}))return fail("Invalid gain value",ProfileReadIssue::GainValue,key);
        }
        return true;
    }

    bool coefficients(){
        auto& p=candidate_.profile;const auto& c=frontend();const auto bins=c.fft_size/2+1;
        const auto grid=source_.value("metadata").toObject().value("grid").toObject();double start=0,step=0;std::size_t declared=0;
        if(grid.value("kind").toString()!="rfft_hz_dc_nyquist" || !number(grid,"start_hz",start) || start!=0 || !number(grid,"step_hz",step) || step!=c.sample_rate/double(c.fft_size) || !count(grid,"bins",declared) || declared!=bins)return fail("Invalid frequency grid",ProfileReadIssue::Layout,"grid");
        if(!coefficient_vector("gains",p.gains,bins) || !coefficient_vector("mean_spectrum",p.mean_spectrum,bins))return false;
        if(source_.contains("frequency_presence") && !vector(source_.value("frequency_presence"),p.frequency_presence,bins,budget_))return fail("Invalid frequency presence");
        return true;
    }
    bool preview_values(const QJsonObject& object,SpectrogramPreview& preview){
        if(!vector(object.value("values"),preview.values,preview.rows*preview.bands,budget_) || !vector(object.value("time_seconds"),preview.time_seconds,preview.rows,budget_) || !vector(object.value("frequency_hz"),preview.frequency_hz,preview.bands,budget_))return fail("Invalid composition preview coordinates/values");
        return valid_preview_coordinates(preview) || fail("Invalid composition preview coordinates");
    }
    bool preview(const QJsonObject& track,std::uint64_t samples){
        if(!track.contains("preview")){candidate_.previews.emplace_back();return true;}
        const auto object=track.value("preview").toObject();SpectrogramPreview image;
        if(!count(object,"rows",image.rows) || !count(object,"bands",image.bands) || image.rows<1 || image.rows>2048 || image.bands<1 || image.bands>2048 || image.bands>frontend().fft_size/2+1 || !number(object,"duration_seconds",image.duration_seconds) || image.duration_seconds!=double(samples)/frontend().sample_rate || object.value("frequency_reduction").toString()!="mean_contiguous_fft_bins")return fail("Invalid composition preview");
        image.source=track.value("source").toString();image.sample_rate=frontend().sample_rate;image.bin_hz=image.sample_rate/frontend().fft_size;
        if(!preview_values(object,image))return false;
        candidate_.previews.push_back(std::move(image));return true;
    }
    bool composition_metadata(const QJsonObject& object,CompositionStatistics& track){
        std::size_t frames=0,active=0,samples=0;
        if(!object.value("source").isString() || !count(object,"frames",frames) || !count(object,"active_frames",active) || !count(object,"samples",samples) || !number(object,"rms",track.rms) || track.rms<0 || active>frames)return fail("Invalid composition statistics");
        track.source=object.value("source").toString().toStdString();track.frames=frames;track.active_frames=active;track.samples=samples;return true;
    }
    bool composition(const QJsonObject& object){
        CompositionStatistics track;
        if(!composition_metadata(object,track))return false;
        const auto bins=frontend().fft_size/2+1;
        if(object.contains("mean_spectrum") && !vector(object.value("mean_spectrum"),track.mean_spectrum,bins,budget_))return fail("Invalid composition spectrum statistics");
        if(object.contains("frequency_presence") && !vector(object.value("frequency_presence"),track.frequency_presence,bins,budget_))return fail("Invalid composition frequency presence");
        if(!preview(object,track.samples))return false;
        candidate_.profile.compositions.push_back(std::move(track));return true;
    }
    bool compositions(){
        const auto value=source_.value("compositions");
        if(!value.isArray() || value.toArray().size()>4096)return fail("Invalid composition provenance");
        for(const auto& item:value.toArray()){
            if(!item.isObject())return fail("Invalid composition statistics");
            if(!composition(item.toObject()))return false;
        }
        return true;
    }
public:
    ProfileDecoder(const QJsonObject& source,QString& error,ProfileReadDiagnostic& diagnostic)
        :source_(source),error_(error),diagnostic_(diagnostic){}
    bool decode(ProfileDocument& output){
        if(!source_.value("version").isDouble() || source_.value("version").toDouble()!=3)return fail("Unsupported profile version");
        candidate_.preserved=source_;
        if(!metadata() || !training() || !coefficients() || !compositions() || !valid_document(candidate_,error_))return false;
        output=std::move(candidate_);diagnostic_={};return true;
    }
};
QJsonObject encode_channels(const ProfileCompatibility& c,QJsonObject object){
    object["policy"]=c.channel_policy==ChannelPolicy::Mean?"mean":"select";object["index"]=double(c.channel_index);return object;
}
QJsonObject encode_resampler(const ProfileCompatibility& c,QJsonObject object){
    object["mode"]=resampler_name(c.resampler_mode);object["quality"]=int(c.resampler_quality);object["design_version"]=int(c.resampler_design_version);return object;
}
QJsonObject encode_grid(const ProfileCompatibility& c,QJsonObject object){
    object["kind"]="rfft_hz_dc_nyquist";object["start_hz"]=0;object["step_hz"]=c.sample_rate/double(c.fft_size);object["bins"]=double(c.fft_size/2+1);return object;
}
QJsonObject encode_metadata(const ProfileCompatibility& c,QJsonObject object){
    object["resampler"]=encode_resampler(c,object.value("resampler").toObject());
    object["channels"]=encode_channels(c,object.value("channels").toObject());object["grid"]=encode_grid(c,object.value("grid").toObject());
    object["sample_rate"]=c.sample_rate;object["fft_size"]=double(c.fft_size);object["window"]=window_name(c.window);
    object["spectrum"]=spectrum_name(c.projection.representation);object["normalization"]=c.projection.compensate?"one_sided_window_corrected_v1":"raw_rfft_v1";
    object["reference"]=c.reference;object["floor_db"]=c.floor_db;object["coherent_gain"]=c.coherent_gain;object["power_gain"]=c.power_gain;
    object["include_dc"]=c.include_dc;object["include_nyquist"]=c.include_nyquist;return object;
}
QJsonObject encode_training(const ProfileTraining& t,QJsonObject object){
    object["silence_rms"]=t.silence_rms;object["min_gain"]=t.min_gain;object["max_gain"]=t.max_gain;object["smoothing_radius"]=double(t.smoothing_radius);
    object["interval_numerator"]=double(t.interval_numerator);object["interval_denominator"]=double(t.interval_denominator);
    object["hop"]=double(t.hop);object["weighting"]="equal_active_composition";return object;
}
QJsonObject encode_preview(const SpectrogramPreview& preview,QJsonObject object){
    object["rows"]=double(preview.rows);object["bands"]=double(preview.bands);object["duration_seconds"]=preview.duration_seconds;object["frequency_reduction"]="mean_contiguous_fft_bins";
    object["values"]=array(preview.values,object.value("values"));object["time_seconds"]=array(preview.time_seconds,object.value("time_seconds"));object["frequency_hz"]=array(preview.frequency_hz,object.value("frequency_hz"));return object;
}
QJsonObject encode_composition(const CompositionStatistics& track,const SpectrogramPreview* preview,QJsonObject object){
    object["source"]=QString::fromStdString(track.source);object["frames"]=double(track.frames);object["active_frames"]=double(track.active_frames);object["samples"]=double(track.samples);object["rms"]=track.rms;
    if(!track.mean_spectrum.empty())object["mean_spectrum"]=array(track.mean_spectrum,object.value("mean_spectrum"));else object.remove("mean_spectrum");
    if(!track.frequency_presence.empty())object["frequency_presence"]=array(track.frequency_presence,object.value("frequency_presence"));else object.remove("frequency_presence");
    if(preview && preview->rows)object["preview"]=encode_preview(*preview,object.value("preview").toObject());else object.remove("preview");
    return object;
}
QJsonArray encode_compositions(const ProfileDocument& document,const QJsonArray& previous){
    QJsonArray tracks;
    for(std::size_t i=0;i<document.profile.compositions.size();++i){
        const auto before=i<std::size_t(previous.size())?previous[qsizetype(i)].toObject():QJsonObject{};
        const auto* preview=i<document.previews.size()?&document.previews[i]:nullptr;
        tracks.append(encode_composition(document.profile.compositions[i],preview,before));
    }
    return tracks;
}
}
bool decode_profile_diagnostic(const QByteArray& bytes,ProfileDocument& output,QString& error,ProfileReadDiagnostic& diagnostic){
    diagnostic={ProfileReadIssue::Encoding,QStringLiteral("profile")};
    if(bytes.size()>max_json_bytes){error="Profile exceeds 1 GiB limit";return false;}
    QJsonParseError parse;const auto document=QJsonDocument::fromJson(bytes,&parse);
    if(parse.error!=QJsonParseError::NoError || !document.isObject()){error="Malformed profile JSON";return false;}
    return ProfileDecoder(document.object(),error,diagnostic).decode(output);
}

bool decode_profile(const QByteArray& bytes,ProfileDocument& output,QString& error){
    ProfileReadDiagnostic diagnostic;return decode_profile_diagnostic(bytes,output,error,diagnostic);
}
QByteArray encode_profile(const ProfileDocument& document){
    const auto& profile=document.profile;auto object=document.preserved;object["version"]=3;
    object["metadata"]=encode_metadata(profile.compatibility,object.value("metadata").toObject());
    object["training"]=encode_training(profile.training,object.value("training").toObject());
    object["gains"]=array(profile.gains,object.value("gains"));object["mean_spectrum"]=array(profile.mean_spectrum,object.value("mean_spectrum"));
    object["compositions"]=encode_compositions(document,object.value("compositions").toArray());
    if(!profile.frequency_presence.empty())object["frequency_presence"]=array(profile.frequency_presence,object.value("frequency_presence"));else object.remove("frequency_presence");
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
bool save_profile(const QString& path,const ProfileDocument& doc,QString& error) {
    if(!valid_document(doc,error))return false;
    const auto bytes=encode_profile(doc);if(bytes.size()>max_json_bytes){error="Profile exceeds 1 GiB limit";return false;}
    QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){error=file.errorString();return false;}return true;
}
bool load_profile_diagnostic(const QString& path,ProfileDocument& output,QString& error,ProfileReadDiagnostic& diagnostic) {
    diagnostic={};
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)){error=file.errorString();return false;}
    if(file.size()>max_json_bytes){error="Profile exceeds 1 GiB limit";diagnostic={ProfileReadIssue::Encoding,QStringLiteral("profile")};return false;}
    const auto bytes=file.read(max_json_bytes+1);
    if(file.error()!=QFileDevice::NoError){error=file.errorString();return false;}
    return decode_profile_diagnostic(bytes,output,error,diagnostic);
}

bool load_profile(const QString& path,ProfileDocument& output,QString& error) {
    ProfileReadDiagnostic diagnostic;return load_profile_diagnostic(path,output,error,diagnostic);
}
}
