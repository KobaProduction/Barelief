#include "profile_codec.h"
#include "music_archive.h"
#include "curve_json.h"
#include <dsp_kernels/frequency_measurements.h>
#include <signal_contracts/spectral_calibration.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>
#include <bit>
#include <type_traits>
#include <algorithm>
#include <utility>

namespace desktop_analysis::offline {
namespace {
constexpr qsizetype max_json_bytes=1024ll*1024*1024;
using namespace dsp_kernels;
bool resampler_mode(const QString& text,ResamplerMode& mode) {
    return signal_contracts::parse_resampler_mode(text.toStdString(),mode);
}
bool number(const QJsonObject& o,const char* key,double& v) {auto x=o.value(key);if(!x.isDouble())return false;v=x.toDouble();return std::isfinite(v);}
bool count(const QJsonObject& o,const char* key,std::size_t& v) {double n;if(!number(o,key,n)||n<0||n>1000000000000.0||std::floor(n)!=n)return false;v=std::size_t(n);return true;}
using array_detail::vector;
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
bool valid_measurement_metadata(const CompositionStatistics& t,double fallback) {
    const auto rate=dsp_kernels::measurement_rate(t,fallback);
    const auto valid_rate=[](double r){return std::isfinite(r) && r>0 && r<=768000 && std::floor(r)==r;};
    return valid_rate(rate) && (!t.source_sample_rate || valid_rate(t.source_sample_rate)) &&
        ((!t.sample_rate && !t.source_sample_rate) || (t.sample_rate>0 && t.source_sample_rate>0)) && t.measured_max_hz<=rate/2;
}
bool read_rms_band(const QJsonObject& o,CompositionStatistics::RmsBand& b) {
    std::size_t index=0,frames=0;
    if(!count(o,"index",index) || index>dsp_kernels::rms_overflow_band || !count(o,"frames",frames) ||
        !number(o,"square_sum",b.square_sum))return false;
    b.index=unsigned(index);b.frames=frames;return true;
}
bool read_band_spectra(const QJsonObject& o,CompositionStatistics::RmsBand& b,std::size_t bins,std::size_t& budget,ProfileArraySource* source) {
    return vector(o.value("mean_spectrum"),b.mean_spectrum,bins,budget,source) && vector(o.value("peak_spectrum"),b.peak_spectrum,bins,budget,source);
}
bool read_rms_bands(const QJsonObject& o,CompositionStatistics& t,std::size_t bins,std::size_t& budget,ProfileArraySource* source) {
    if(o.value("version").toDouble()!=1 || !o.value("bands").isArray())return false;
    const auto bands=o.value("bands").toArray();
    if(bands.size()>dsp_kernels::rms_overflow_band+1 || std::size_t(bands.size())>budget/sizeof(CompositionStatistics::RmsBand))return false;
    budget-=std::size_t(bands.size())*sizeof(CompositionStatistics::RmsBand);t.rms_statistics_version=1;
    for(const auto& value:bands){
        CompositionStatistics::RmsBand band;
        if(!value.isObject() || !read_rms_band(value.toObject(),band) || !read_band_spectra(value.toObject(),band,bins,budget,source))return false;
        t.rms_bands.push_back(std::move(band));
    }
    return dsp_kernels::valid_rms_bands(t);
}
bool read_measurements(const QJsonObject& o,CompositionStatistics& t,std::size_t bins,std::size_t& budget,ProfileArraySource* source) {
    if(o.contains("analysis")){
        const auto a=o.value("analysis").toObject();
        if(!number(a,"source_sample_rate",t.source_sample_rate) || !number(a,"sample_rate",t.sample_rate))return false;
    }
    return !o.contains("rms_statistics") || read_rms_bands(o.value("rms_statistics").toObject(),t,bins,budget,source);
}
bool valid_composition(const CompositionStatistics& track,std::size_t bins) {
    if(!dsp_kernels::valid_rms_bands(track))return false;
    if(track.quiet_frames>track.active_frames || track.digital_silence_frames>track.frames-track.active_frames ||
        !std::isfinite(track.quiet_rms) || track.quiet_rms<0 || !std::isfinite(track.measured_max_hz) || track.measured_max_hz<0 ||
        !std::isfinite(track.minimum_frame_rms) || track.minimum_frame_rms<0 ||
        !std::isfinite(track.maximum_frame_rms) || track.maximum_frame_rms<track.minimum_frame_rms)return false;
    if((!track.quiet_mean_spectrum.empty() && track.quiet_mean_spectrum.size()!=bins) ||
        (!track.quiet_peak_spectrum.empty() && track.quiet_peak_spectrum.size()!=bins) ||
        !valid_values(track.quiet_mean_spectrum) || !valid_values(track.quiet_peak_spectrum))return false;
    if(track.active_frames>track.frames || !std::isfinite(track.rms) || track.rms<0 || (!track.mean_spectrum.empty() && track.mean_spectrum.size()!=bins) || (!track.peak_spectrum.empty() && track.peak_spectrum.size()!=bins) || (!track.frequency_presence.empty() && track.frequency_presence.size()!=bins))return false;
    return valid_values(track.mean_spectrum) && valid_values(track.peak_spectrum) && valid_values(track.frequency_presence) &&
        std::none_of(track.frequency_presence.begin(),track.frequency_presence.end(),[](double value){return value>1;});
}
bool valid_preview(const SpectrogramPreview& image,const CompositionStatistics& track,const ProfileCompatibility& frontend,std::size_t bins) {
    if(image.rows>2048 || image.bands<1 || image.bands>2048 || image.bands>bins || image.values.size()!=image.rows*image.bands)return false;
    const auto rate=dsp_kernels::measurement_rate(track,frontend.sample_rate);
    if(image.source!=QString::fromStdString(track.source) || image.sample_rate!=rate || image.bin_hz!=rate/frontend.fft_size || image.duration_seconds!=double(track.samples)/rate)return false;
    return valid_values(image.values) && valid_preview_coordinates(image);
}
std::size_t statistics_bytes(const CompositionStatistics& track) {
    auto bytes=(track.mean_spectrum.size()+track.peak_spectrum.size()+track.frequency_presence.size()+track.quiet_mean_spectrum.size()+track.quiet_peak_spectrum.size())*sizeof(double);
    bytes+=track.rms_bands.size()*sizeof(CompositionStatistics::RmsBand);
    for(const auto& band:track.rms_bands)bytes+=(band.mean_spectrum.size()+band.peak_spectrum.size())*sizeof(float);
    return bytes;
}
bool accumulate_saved_preview(const ProfileDocument& document,std::size_t index,std::size_t& bytes,QString& error) {
    if(index>=document.previews.size() || !document.previews[index].rows)return true;
    const auto& image=document.previews[index];const auto& profile=document.profile;
    if(!valid_preview(image,profile.compositions[index],profile.compatibility,profile.compatibility.fft_size/2+1)){error="Invalid saved composition preview";return false;}
    bytes+=image.values.size()*sizeof(float)+(image.time_seconds.size()+image.frequency_hz.size())*sizeof(double);
    return true;
}
bool validate_saved_composition(const ProfileDocument& document,std::size_t index,std::size_t& bytes,QString& error) {
    const auto& profile=document.profile;const auto& track=profile.compositions[index];
    const auto bins=profile.compatibility.fft_size/2+1;
    if(!valid_composition(track,bins) || !valid_measurement_metadata(track,profile.compatibility.sample_rate)){error="Invalid saved composition statistics";return false;}
    bytes+=statistics_bytes(track);
    if(!accumulate_saved_preview(document,index,bytes,error))return false;
    if(bytes>1024ull*1024*1024){error="Decoded profile exceeds 1 GiB limit";return false;}
    return true;
}
bool valid_document_coefficients(const FrequencyProfile& profile,QString& error) {
    std::string validation;
    if(!validate_profile(profile,profile.compatibility,validation)){error=QString::fromStdString(validation);return false;}
    if(!valid_frequency_curve(profile)){error="Invalid frequency function";return false;}
    return true;
}
bool valid_diagnostics(const QJsonObject& root) {
    if(!root.contains("diagnostics"))return true;
    if(!root.value("diagnostics").isObject())return false;
    const auto diagnostics=root.value("diagnostics").toObject();
    if(!diagnostics.contains("quiet_db"))return true;
    double db=0;
    return number(diagnostics,"quiet_db",db) && db>=-200 && db<=0 && std::fmod(db,2.)==0;
}
bool valid_measurement_collection(const ProfileDocument& document,QString& error) {
    if(!valid_diagnostics(document.preserved)){error="Invalid quiet-frame diagnostic level";return false;}
    const auto& profile=document.profile;
    if(profile.compositions.size()>4096 || (!document.previews.empty() && document.previews.size()!=profile.compositions.size())){error="Invalid saved composition count";return false;}
    return true;
}
bool valid_document_header(const ProfileDocument& document,QString& error) {
    return valid_measurement_collection(document,error) && valid_document_coefficients(document.profile,error);
}
bool valid_document(const ProfileDocument& document,QString& error) {
    if(!valid_document_header(document,error))return false;
    const auto& profile=document.profile;
    std::size_t bytes=profile.gains.size()*sizeof(float)+(profile.mean_spectrum.size()+profile.peak_spectrum.size()+profile.frequency_presence.size())*sizeof(double);
    for(std::size_t i=0;i<profile.compositions.size();++i)if(!validate_saved_composition(document,i,bytes,error))return false;
    return true;
}
}
namespace {
class ProfileDecoder {
    const QJsonObject& source_;
    ProfileArraySource* arrays_;
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
    bool spectral_layout(const QJsonObject& object){
        auto& c=frontend();
        if(!number(object,"coherent_gain",c.coherent_gain) || c.coherent_gain<0)return fail("Invalid coherent gain",ProfileReadIssue::CoherentGain,"coherent_gain");
        if(!number(object,"power_gain",c.power_gain) || c.power_gain<0)return fail("Invalid power gain",ProfileReadIssue::PowerGain,"power_gain");
        if(!object.value("include_dc").isBool() || !object.value("include_nyquist").isBool())return fail("Missing explicit spectral layout",ProfileReadIssue::Layout,"include_dc");
        c.include_dc=object.value("include_dc").toBool();c.include_nyquist=object.value("include_nyquist").toBool();
        return true;
    }
    bool metadata(){
        const auto object=source_.value("metadata").toObject();auto& c=frontend();
        if(!number(object,"sample_rate",c.sample_rate) || c.sample_rate<=0)return fail("Invalid sample rate",ProfileReadIssue::SampleRate,"sample_rate");
        if(!count(object,"fft_size",c.fft_size) || c.fft_size<2 || (c.fft_size&(c.fft_size-1)))return fail("Invalid FFT size",ProfileReadIssue::FftSize,"fft_size");
        if(!number(object,"reference",c.reference) || !number(object,"floor_db",c.floor_db))return fail("Missing profile metadata",ProfileReadIssue::Frontend,"reference");
        if(!spectral_layout(object))return false;
        return resampler(object) && channels(object) && projection(object);
    }
    bool training_gain_mode(const QJsonObject& object){
        auto& t=candidate_.profile.training;
        const auto mode=object.value("gain_mode");
        if(mode.isUndefined() || mode.toString()=="mean_equalization")t.gain_mode=ProfileGainMode::MeanEqualization;
        else if(mode.toString()=="peak_normalization")t.gain_mode=ProfileGainMode::PeakNormalization;
        else return fail("Unknown frequency gain mode",ProfileReadIssue::Frontend,"gain_mode");
        return true;
    }
    bool validate_training(){
        std::string validation;
        if(valid_profile_settings(frontend(),candidate_.profile.training,validation))return true;
        error_=QString::fromStdString(validation);diagnostic_={ProfileReadIssue::Frontend,QStringLiteral("training")};return false;
    }
    bool training(){
        const auto object=source_.value("training").toObject();auto& t=candidate_.profile.training;
        if(!number(object,"silence_rms",t.silence_rms) || !number(object,"min_gain",t.min_gain) || !number(object,"max_gain",t.max_gain) || !count(object,"smoothing_radius",t.smoothing_radius) || !count(object,"hop",t.hop) || object.value("weighting").toString()!="equal_active_composition")return fail("Invalid training settings");
        std::size_t numerator=0,denominator=0;
        if(!count(object,"interval_numerator",numerator) || !count(object,"interval_denominator",denominator))return fail("Missing analysis cadence");
        t.interval_numerator=numerator;t.interval_denominator=denominator;
        return training_gain_mode(object) && validate_training();
    }
    template<class T> bool binary_coefficients(const QJsonObject& value,const char* key,std::vector<T>& output,std::size_t bins) {
        QByteArray raw;
        if(bins>budget_/sizeof(T) || !array_detail::unpack(value,bins,sizeof(T),raw,arrays_))return fail("Invalid coefficient encoding",ProfileReadIssue::Encoding,key);
        std::vector<T> decoded;decoded.reserve(bins);
        if(!array_detail::binary(raw,decoded,bins))return fail("Invalid coefficient value",ProfileReadIssue::GainValue,key);
        budget_-=bins*sizeof(T);output=std::move(decoded);return true;
    }

    template<class T> bool object_coefficients(const QJsonObject& value,const char* key,std::vector<T>& output,std::size_t bins) {
        std::size_t declared=0;
        if(!count(value,"count",declared))return fail("Invalid coefficient encoding",ProfileReadIssue::Encoding,key);
        if(declared!=bins)return fail("Wrong coefficient count",ProfileReadIssue::GainCount,key);
        return binary_coefficients(value,key,output,bins);
    }
    template<class T> bool coefficient_payload(const QJsonValue& value,const char* key,std::vector<T>& output,std::size_t bins) {
        if(value.isArray()){
            if(std::size_t(value.toArray().size())!=bins)return fail("Wrong coefficient count",ProfileReadIssue::GainCount,key);
            return vector(value,output,bins,budget_,arrays_) || fail("Invalid profile coefficients",ProfileReadIssue::GainValue,key);
        }
        if(value.isObject())return object_coefficients(value.toObject(),key,output,bins);
        return fail("Invalid coefficient encoding",ProfileReadIssue::Encoding,key);
    }
    template<class T> bool coefficient_vector(const char* key,std::vector<T>& output,std::size_t bins) {
        if(!coefficient_payload(source_.value(QString::fromUtf8(key)),key,output,bins))return false;
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
        if(source_.contains("peak_spectrum") && !vector(source_.value("peak_spectrum"),p.peak_spectrum,bins,budget_,arrays_))return fail("Invalid peak spectrum");
        if(source_.contains("frequency_presence") && !vector(source_.value("frequency_presence"),p.frequency_presence,bins,budget_,arrays_))return fail("Invalid frequency presence");
        return true;
    }
    bool preview_values(const QJsonObject& object,SpectrogramPreview& preview){
        if(!vector(object.value("values"),preview.values,preview.rows*preview.bands,budget_,arrays_) || !vector(object.value("time_seconds"),preview.time_seconds,preview.rows,budget_,arrays_) || !vector(object.value("frequency_hz"),preview.frequency_hz,preview.bands,budget_,arrays_))return fail("Invalid composition preview coordinates/values");
        return valid_preview_coordinates(preview) || fail("Invalid composition preview coordinates");
    }
    bool preview(const QJsonObject& track,const CompositionStatistics& statistics){
        if(!track.contains("preview")){candidate_.previews.emplace_back();return true;}
        const auto object=track.value("preview").toObject();SpectrogramPreview image;
        const auto rate=dsp_kernels::measurement_rate(statistics,frontend().sample_rate);
        if(!count(object,"rows",image.rows) || !count(object,"bands",image.bands) || image.rows<1 || image.rows>2048 || image.bands<1 || image.bands>2048 || image.bands>frontend().fft_size/2+1 || !number(object,"duration_seconds",image.duration_seconds) || image.duration_seconds!=double(statistics.samples)/rate || object.value("frequency_reduction").toString()!="mean_contiguous_fft_bins")return fail("Invalid composition preview");
        image.source=track.value("source").toString();image.sample_rate=rate;image.bin_hz=image.sample_rate/frontend().fft_size;
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
        if(!composition_spectra(object,track,bins))return false;
        if(!read_measurements(object,track,bins,budget_,arrays_))return fail("Invalid per-track measurement metadata");
        if(!preview(object,track))return false;
        candidate_.profile.compositions.push_back(std::move(track));return true;
    }
    bool composition_spectra(const QJsonObject& object,CompositionStatistics& track,std::size_t bins){
        if(object.contains("mean_spectrum") && !vector(object.value("mean_spectrum"),track.mean_spectrum,bins,budget_,arrays_))return fail("Invalid composition spectrum statistics");
        if(object.contains("peak_spectrum") && !vector(object.value("peak_spectrum"),track.peak_spectrum,bins,budget_,arrays_))return fail("Invalid composition peak spectrum");
        if(object.contains("frequency_presence") && !vector(object.value("frequency_presence"),track.frequency_presence,bins,budget_,arrays_))return fail("Invalid composition frequency presence");
        return quiet_statistics(object,track,bins);
    }
    bool quiet_statistics(const QJsonObject& object,CompositionStatistics& track,std::size_t bins){
        if(!object.contains("quiet_statistics"))return true;
        const auto quiet=object.value("quiet_statistics").toObject();std::size_t silent=0,counted=0;
        if(!count(quiet,"digital_silence_frames",silent) || !count(quiet,"quiet_frames",counted) ||
            !number(quiet,"quiet_rms",track.quiet_rms) || !number(quiet,"minimum_frame_rms",track.minimum_frame_rms) ||
            !number(quiet,"maximum_frame_rms",track.maximum_frame_rms) || !number(quiet,"measured_max_hz",track.measured_max_hz))return fail("Invalid quiet-frame statistics");
        track.digital_silence_frames=silent;track.quiet_frames=counted;
        return vector(quiet.value("mean_spectrum"),track.quiet_mean_spectrum,bins,budget_,arrays_) &&
            vector(quiet.value("peak_spectrum"),track.quiet_peak_spectrum,bins,budget_,arrays_);
    }
    bool compositions(){
        const auto value=source_.value("compositions");
        if(!value.isArray() || value.toArray().size()>4096)return fail("Invalid composition provenance");
        const auto items=value.toArray();
        for(const auto& item:items){
            if(!item.isObject())return fail("Invalid composition statistics");
            if(!composition(item.toObject()))return false;
        }
        return true;
    }
public:
    ProfileDecoder(const QJsonObject& source,QString& error,ProfileReadDiagnostic& diagnostic,ProfileArraySource* arrays)
        :source_(source),arrays_(arrays),error_(error),diagnostic_(diagnostic){}
    bool decode(ProfileDocument& output){
        const auto version=source_.value("version").toDouble();
        if(!source_.value("version").isDouble() || (version!=3 && version!=4 && version!=5))return fail("Unsupported profile version");
        candidate_.preserved=source_;
        if(!metadata() || !training() || !coefficients() || !compositions())return false;
        if(version==4 && (source_.value("kind").toString()!="barelief.frequency_profile" || !decode_curve(source_.value("function").toObject(),candidate_.profile)))return fail("Invalid frequency function");
        if(version==5 && !function_v5())return false;
        if(!valid_document(candidate_,error_))return false;
        output=std::move(candidate_);diagnostic_={};return true;
    }
    bool function_v5(){
        if(source_.value("kind").toString()!="barelief.frequency_profile")return fail("Invalid frequency profile kind");
        return !source_.contains("function") || decode_curve(source_.value("function").toObject(),candidate_.profile) || fail("Invalid frequency function");
    }
};
ProfileDocument ready_document(const ProfileDocument& source){
    if(!source.profile.curve)return source;
    ProfileDocument result;const auto& p=source.profile;auto& r=result.profile;
    r.compatibility=p.compatibility;r.training=p.training;r.curve=p.curve;
    r.gains=p.gains;r.mean_spectrum=p.mean_spectrum;r.peak_spectrum=p.peak_spectrum;r.frequency_presence=p.frequency_presence;
    result.archive_preserved=source.archive_preserved;result.preserved=source.preserved;result.preserved.remove("compositions");return result;
}
}
bool decode_profile_object(const QJsonObject& object,ProfileDocument& output,QString& error,ProfileReadDiagnostic& diagnostic,ProfileArraySource* source){
    return ProfileDecoder(object,error,diagnostic,source).decode(output);
}
bool valid_profile_storage(const ProfileDocument& document,QString& error){return valid_document(document,error);}
bool valid_scan_measurements(const ProfileDocument& document,QString& error) {
    if(!valid_measurement_collection(document,error))return false;
    std::size_t bytes=0;
    for(std::size_t i=0;i<document.profile.compositions.size();++i)if(!validate_saved_composition(document,i,bytes,error))return false;
    return true;
}
bool decode_profile_diagnostic(const QByteArray& bytes,ProfileDocument& output,QString& error,ProfileReadDiagnostic& diagnostic){
    diagnostic={ProfileReadIssue::Encoding,QStringLiteral("profile")};
    if(bytes.size()>max_json_bytes){error="Profile exceeds 1 GiB limit";return false;}
    QJsonParseError parse;const auto document=QJsonDocument::fromJson(bytes,&parse);
    if(parse.error!=QJsonParseError::NoError || !document.isObject()){error="Malformed profile JSON";return false;}
    return decode_profile_object(document.object(),output,error,diagnostic);
}

bool decode_profile(const QByteArray& bytes,ProfileDocument& output,QString& error){
    ProfileReadDiagnostic diagnostic;return decode_profile_diagnostic(bytes,output,error,diagnostic);
}
QByteArray encode_profile(const ProfileDocument& document){
    return QJsonDocument(encode_profile_object(document)).toJson(QJsonDocument::Compact);
}
bool save_profile(const QString& path,const ProfileDocument& doc,QString& error) {
    if(!valid_document(doc,error))return false;
    const auto ready=ready_document(doc);
    if(!path.endsWith(".json",Qt::CaseInsensitive))return save_music_archive(path,ready,MusicArchiveKind::Profile,error);
    const auto bytes=encode_profile(ready);if(bytes.size()>max_json_bytes){error="Profile exceeds 1 GiB limit";return false;}
    QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()){error=file.errorString();return false;}
    return true;
}
bool load_profile_diagnostic(const QString& path,ProfileDocument& output,QString& error,ProfileReadDiagnostic& diagnostic) {
    diagnostic={};
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)){error=file.errorString();return false;}
    if(file.peek(4)==QByteArray("PK\x03\x04",4))return load_music_archive(path,MusicArchiveKind::Profile,output,error,diagnostic);
    if(file.size()>max_json_bytes){error="Profile exceeds 1 GiB limit";diagnostic={ProfileReadIssue::Encoding,QStringLiteral("profile")};return false;}
    const auto bytes=file.read(max_json_bytes+1);
    if(file.error()!=QFileDevice::NoError){error=file.errorString();return false;}
    return decode_profile_diagnostic(bytes,output,error,diagnostic);
}

bool load_profile(const QString& path,ProfileDocument& output,QString& error) {
    ProfileReadDiagnostic diagnostic;return load_profile_diagnostic(path,output,error,diagnostic);
}
}
