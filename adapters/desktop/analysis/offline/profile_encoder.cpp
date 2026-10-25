#include "profile_codec.h"
#include "curve_json.h"
#include <signal_contracts/spectral_calibration.h>
#include <QJsonDocument>
#include <algorithm>

namespace desktop_analysis::offline {
namespace {
using namespace dsp_kernels;
QString window_name(WindowFunction w){return QString::fromUtf8(window_function_name(w));}
QString spectrum_name(SpectrumProjection s){return QString::fromUtf8(spectrum_projection_name(s));}
QString resampler_name(ResamplerMode m){return QString::fromUtf8(signal_contracts::resampler_mode_name(m));}
class ProfileEncoder {

    ProfileArraySink* sink_;
    QString prefix_="profile";
    template<class T> QJsonObject array(const std::vector<T>& values,const QJsonValue& previous,const char* name) {
        return array_detail::array(values,previous,sink_,0,prefix_+"/"+QString::fromLatin1(name));
    }
    using Band=CompositionStatistics::RmsBand;
    QJsonObject grouped_spectrum(const CompositionStatistics& t,std::vector<float> Band::* member,const char* name) {
        if(!sink_ || t.rms_bands.empty())return {};
        const auto bins=t.mean_spectrum.size();std::vector<float> values;values.reserve(t.rms_bands.size()*bins);
        for(const auto& band:t.rms_bands){const auto& spectrum=band.*member;values.insert(values.end(),spectrum.begin(),spectrum.end());}
        return array_detail::array(values,{},sink_,bins,prefix_+"/rms/"+QString::fromLatin1(name));
    }
    QJsonObject band_reference(QJsonObject previous,const QJsonObject& matrix,std::size_t row,std::size_t bins) {
        for(const auto* key:{"encoding","entry"})previous[key]=matrix.value(key);
        previous.remove("data");previous["count"]=double(bins);previous["offset"]=double(row*bins);return previous;
    }
    void band_spectra(QJsonObject& o,const Band& b,std::size_t row,const QJsonObject& means,const QJsonObject& peaks) {
        if(!sink_){o["mean_spectrum"]=array(b.mean_spectrum,o.value("mean_spectrum"),"rms/mean_spectrum");o["peak_spectrum"]=array(b.peak_spectrum,o.value("peak_spectrum"),"rms/peak_spectrum");return;}
        o["mean_spectrum"]=band_reference(o.value("mean_spectrum").toObject(),means,row,b.mean_spectrum.size());
        o["peak_spectrum"]=band_reference(o.value("peak_spectrum").toObject(),peaks,row,b.peak_spectrum.size());
    }
    void encode_coefficients(const FrequencyProfile& p,QJsonObject& o) {
        o["gains"]=array(p.gains,o.value("gains"),"gains");o["mean_spectrum"]=array(p.mean_spectrum,o.value("mean_spectrum"),"mean_spectrum");
        encode_optional_array(o,"peak_spectrum",p.peak_spectrum);encode_optional_array(o,"frequency_presence",p.frequency_presence);
    }
QJsonArray encode_rms_bands(const CompositionStatistics& t,const QJsonArray& previous) {
    QJsonArray bands;const auto means=grouped_spectrum(t,&Band::mean_spectrum,"mean_spectrum"),peaks=grouped_spectrum(t,&Band::peak_spectrum,"peak_spectrum");
    for(std::size_t i=0;i<t.rms_bands.size();++i){
        auto o=i<std::size_t(previous.size())?previous[qsizetype(i)].toObject():QJsonObject{};const auto& b=t.rms_bands[i];
        o["index"]=int(b.index);o["frames"]=double(b.frames);o["square_sum"]=b.square_sum;
        band_spectra(o,b,i,means,peaks);bands.append(o);
    }
    return bands;
}
void encode_measurements(const CompositionStatistics& t,QJsonObject& o) {
    if(t.sample_rate){
        auto a=o.value("analysis").toObject();a["source_sample_rate"]=t.source_sample_rate;a["sample_rate"]=t.sample_rate;o["analysis"]=a;
    }
    if(t.rms_statistics_version){
        auto r=o.value("rms_statistics").toObject();r["version"]=int(t.rms_statistics_version);
        r["bands"]=encode_rms_bands(t,r.value("bands").toArray());o["rms_statistics"]=r;
    }
}
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
    object["hop"]=double(t.hop);object["weighting"]="equal_active_composition";
    object["gain_mode"]=t.gain_mode==ProfileGainMode::PeakNormalization?"peak_normalization":"mean_equalization";
    return object;
}
QJsonObject encode_preview(const SpectrogramPreview& preview,QJsonObject object){
    object["rows"]=double(preview.rows);object["bands"]=double(preview.bands);object["duration_seconds"]=preview.duration_seconds;object["frequency_reduction"]="mean_contiguous_fft_bins";
    object["values"]=array(preview.values,object.value("values"),"preview/values");object["time_seconds"]=array(preview.time_seconds,object.value("time_seconds"),"preview/time_seconds");object["frequency_hz"]=array(preview.frequency_hz,object.value("frequency_hz"),"preview/frequency_hz");return object;
}
template<class T> void encode_optional_array(QJsonObject& object,const char* key,const std::vector<T>& values){
    if(values.empty())object.remove(key);
    else object[key]=array(values,object.value(key),key);
}
void encode_quiet_statistics(const CompositionStatistics& track,QJsonObject& object){
    if(!track.quiet_mean_spectrum.empty()){
        auto quiet=object.value("quiet_statistics").toObject();
        quiet["digital_silence_frames"]=double(track.digital_silence_frames);quiet["quiet_frames"]=double(track.quiet_frames);
        quiet["quiet_rms"]=track.quiet_rms;quiet["minimum_frame_rms"]=track.minimum_frame_rms;quiet["maximum_frame_rms"]=track.maximum_frame_rms;quiet["measured_max_hz"]=track.measured_max_hz;
        quiet["mean_spectrum"]=array(track.quiet_mean_spectrum,quiet.value("mean_spectrum"),"quiet/mean_spectrum");quiet["peak_spectrum"]=array(track.quiet_peak_spectrum,quiet.value("peak_spectrum"),"quiet/peak_spectrum");
        object["quiet_statistics"]=quiet;
    }
}
QJsonObject encode_composition(const CompositionStatistics& track,const SpectrogramPreview* preview,QJsonObject object){
    object["source"]=QString::fromStdString(track.source);object["frames"]=double(track.frames);object["active_frames"]=double(track.active_frames);object["samples"]=double(track.samples);object["rms"]=track.rms;
    encode_optional_array(object,"mean_spectrum",track.mean_spectrum);
    encode_optional_array(object,"peak_spectrum",track.peak_spectrum);
    encode_optional_array(object,"frequency_presence",track.frequency_presence);encode_quiet_statistics(track,object);
    encode_measurements(track,object);
    if(preview && preview->rows)object["preview"]=encode_preview(*preview,object.value("preview").toObject());else object.remove("preview");
    return object;
}
QJsonArray encode_compositions(const ProfileDocument& document,const QJsonArray& previous){
    QJsonArray tracks;
    for(std::size_t i=0;i<document.profile.compositions.size();++i){
        const auto before=i<std::size_t(previous.size())?previous[qsizetype(i)].toObject():QJsonObject{};
        const auto* preview=i<document.previews.size()?&document.previews[i]:nullptr;
        prefix_="tracks/"+QString::number(i);
        tracks.append(encode_composition(document.profile.compositions[i],preview,before));
    }
    return tracks;
}
void encode_function(const FrequencyProfile& profile,QJsonObject& object){
    const auto measured=std::ranges::any_of(profile.compositions,[](const auto& t){return t.sample_rate>0 || t.rms_statistics_version;});
    object["version"]=measured?5:profile.curve?4:3;
    if(measured)object["kind"]="barelief.frequency_profile";
    if(profile.curve){object["kind"]="barelief.frequency_profile";object["function"]=encode_curve(*profile.curve,object.value("function").toObject());}
}
public:
    explicit ProfileEncoder(ProfileArraySink* sink):sink_(sink){}
QJsonObject encode(const ProfileDocument& document,bool coefficients){
    const auto& profile=document.profile;auto object=document.preserved;encode_function(profile,object);
    object["metadata"]=encode_metadata(profile.compatibility,object.value("metadata").toObject());
    object["training"]=encode_training(profile.training,object.value("training").toObject());
    if(coefficients)encode_coefficients(profile,object);
    object["compositions"]=encode_compositions(document,object.value("compositions").toArray());
    return object;
}
};
}
QJsonObject encode_profile_object(const ProfileDocument& document,ProfileArraySink* sink,bool coefficients){return ProfileEncoder(sink).encode(document,coefficients);}
}
