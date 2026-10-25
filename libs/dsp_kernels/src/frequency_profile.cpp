#include <signal_contracts/spectral_calibration.h>
#include <dsp_kernels/frequency_profile.h>
#include <dsp_kernels/sample_cadence.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include "frequency_curve_math.h"
#include <dsp_kernels/frequency_measurements.h>

namespace dsp_kernels {
WindowScaling profile_window_scaling(const ProfileCompatibility& c) noexcept {
    return {c.fft_size,c.coherent_gain>0?c.coherent_gain:window_sum(c.fft_size,c.window),
            c.power_gain>0?c.power_gain:window_square_sum(c.fft_size,c.window)};
}

namespace {
bool valid_frontend(const ProfileCompatibility& c) {
    return std::isfinite(c.sample_rate) && c.sample_rate>0 && c.sample_rate<=768000 && std::floor(c.sample_rate)==c.sample_rate &&
        c.fft_size>=512 && c.fft_size<=max_fft_samples && !(c.fft_size&(c.fft_size-1)) &&
        !window_function_name(c.window).empty() && is_amplitude_spectrum_projection(c.projection.representation);
}
bool valid_calibration(const ProfileCompatibility& c) {
    return std::isfinite(c.coherent_gain) && c.coherent_gain>=0 && std::isfinite(c.power_gain) && c.power_gain>=0 &&
        c.include_dc && c.include_nyquist && std::isfinite(c.reference) && c.reference>0 &&
        std::isfinite(c.floor_db) && c.floor_db<=0 && c.floor_db>=-300;
}
bool valid_channel_and_resampler(const ProfileCompatibility& c) {
    return (c.channel_policy==ChannelPolicy::Mean || c.channel_policy==ChannelPolicy::Select) && c.channel_index<=255 &&
        c.resampler_quality<=10 && c.resampler_design_version==PreparedResampler::design_version &&
        !signal_contracts::resampler_mode_name(c.resampler_mode).empty();
}
bool valid_training(const ProfileCompatibility& c,const ProfileTraining& t) {
    return t.interval_denominator && std::isfinite(t.silence_rms) && t.silence_rms>=0 && t.silence_rms<=1 &&
        std::isfinite(t.min_gain) && std::isfinite(t.max_gain) && t.min_gain>0 && t.max_gain>=t.min_gain && t.max_gain<=1000000 &&
        t.smoothing_radius<=c.fft_size/2 && (t.interval_numerator || (t.hop>0 && t.hop<=c.fft_size));
}
using Reason=ProfileMismatchReason;
void frontend_mismatches(const ProfileCompatibility& a,const ProfileCompatibility& b,std::vector<Reason>& reasons) {
    if(a.sample_rate!=b.sample_rate)reasons.push_back(Reason::SampleRate);
    if(a.fft_size!=b.fft_size)reasons.push_back(Reason::FftSize);
    if(a.window!=b.window)reasons.push_back(Reason::Window);
    if(a.projection!=b.projection)reasons.push_back(Reason::Projection);
    if(profile_window_scaling(a).sum!=profile_window_scaling(b).sum)reasons.push_back(Reason::CoherentGain);
    if(profile_window_scaling(a).sum_squares!=profile_window_scaling(b).sum_squares)reasons.push_back(Reason::PowerGain);
}
void resampler_mismatches(const ProfileCompatibility& a,const ProfileCompatibility& b,std::vector<Reason>& reasons) {
    if(a.resampler_mode!=b.resampler_mode)reasons.push_back(Reason::ResamplerMode);
    if(a.resampler_quality!=b.resampler_quality)reasons.push_back(Reason::ResamplerQuality);
    if(a.resampler_design_version!=b.resampler_design_version)reasons.push_back(Reason::ResamplerDesignVersion);
}
void policy_mismatches(const ProfileCompatibility& a,const ProfileCompatibility& b,std::vector<Reason>& reasons) {
    if(a.include_dc!=b.include_dc)reasons.push_back(Reason::DcLayout);
    if(a.include_nyquist!=b.include_nyquist)reasons.push_back(Reason::NyquistLayout);
    if(a.channel_policy!=b.channel_policy)reasons.push_back(Reason::ChannelPolicy);
    if(a.channel_index!=b.channel_index)reasons.push_back(Reason::ChannelIndex);
    resampler_mismatches(a,b,reasons);
}
void calibration_mismatches(const ProfileCompatibility& actual,const ProfileCompatibility& expected,ProfileMatchScope scope,std::vector<Reason>& reasons) {
    if(scope==ProfileMatchScope::Exact){
        if(actual.reference!=expected.reference)reasons.push_back(Reason::Reference);
        if(actual.floor_db!=expected.floor_db)reasons.push_back(Reason::Floor);
    }
}
bool valid_presence(const FrequencyProfile& p) {
    return p.frequency_presence.empty() || (p.frequency_presence.size()==p.gains.size() &&
        std::all_of(p.frequency_presence.begin(),p.frequency_presence.end(),[](double v){return std::isfinite(v) && v>=0 && v<=1;}));
}
bool valid_coefficients(const FrequencyProfile& p) {
    if(p.mean_spectrum.size()!=p.gains.size())return false;
    if(!p.peak_spectrum.empty() && p.peak_spectrum.size()!=p.gains.size())return false;
    if(p.training.gain_mode==ProfileGainMode::PeakNormalization && p.peak_spectrum.size()!=p.gains.size())return false;
    for(std::size_t k=0;k<p.gains.size();++k)
        if(!signal_contracts::valid_profile_gain(p.gains[k]) ||
            p.gains[k]<(p.training.gain_mode==ProfileGainMode::PeakNormalization?1.:p.training.min_gain)*(1-1e-6) ||
            p.gains[k]>p.training.max_gain*(1+1e-6) ||
            !std::isfinite(p.mean_spectrum[k]) || p.mean_spectrum[k]<0 ||
            (!p.peak_spectrum.empty() && (!std::isfinite(p.peak_spectrum[k]) || p.peak_spectrum[k]<0)))return false;
    return true;
}
bool valid_expected_frontend(ProfileCompatibility expected,const ProfileCompatibility& stored,ProfileMatchScope scope) {
    if(scope==ProfileMatchScope::SignalOnly){expected.reference=stored.reference;expected.floor_db=stored.floor_db;}
    return valid_frontend(expected) && valid_calibration(expected) && valid_channel_and_resampler(expected);
}
void payload_mismatches(const FrequencyProfile& profile,std::size_t bins,std::vector<Reason>& reasons) {
    if(profile.gains.size()!=bins || profile.mean_spectrum.size()!=profile.gains.size())reasons.push_back(Reason::FrequencyGridLength);
    if(!valid_presence(profile))reasons.push_back(Reason::FrequencyPresence);
    if(!valid_coefficients(profile))reasons.push_back(Reason::InvalidCoefficient);
}
std::optional<Reason> settings_problem(const ProfileCompatibility& c,const ProfileTraining& training) {
    SampleCadence cadence;
    if(!valid_frontend(c) || !valid_calibration(c) || !valid_channel_and_resampler(c) || !valid_training(c,training))return Reason::InvalidSettings;
    if(training.interval_numerator && (!cadence.prepare(std::uint32_t(c.sample_rate),training.interval_numerator,training.interval_denominator) || cadence.base+(cadence.fraction?1:0)>c.fft_size))return Reason::InvalidCadence;
    return {};
}

}
bool valid_profile_settings(const ProfileCompatibility& c,const ProfileTraining& training,std::string& error) {
    const auto reason=settings_problem(c,training);
    if(reason==Reason::InvalidSettings){error="Invalid frequency profile settings";return false;}
    if(reason==Reason::InvalidCadence){error="Analysis cadence exceeds the declared window";return false;}
    return true;
}
std::vector<ProfileMismatchReason> profile_mismatches(const FrequencyProfile& p,const ProfileCompatibility& expected,ProfileMatchScope scope) {
    std::vector<Reason> reasons;
    const auto problem=settings_problem(p.compatibility,p.training);
    if(problem){reasons.push_back(*problem);return reasons;}
    if(!valid_expected_frontend(expected,p.compatibility,scope))return {Reason::InvalidSettings};
    frontend_mismatches(p.compatibility,expected,reasons);policy_mismatches(p.compatibility,expected,reasons);
    calibration_mismatches(p.compatibility,expected,scope,reasons);
    payload_mismatches(p,expected.fft_size/2+1,reasons);
    return reasons;
}
bool validate_profile(const FrequencyProfile& p,const ProfileCompatibility& expected,std::string& error) {
    const auto reasons=profile_mismatches(p,expected);
    if(reasons.empty())return true;
    error="Invalid frequency profile or declared spectrum/frontend mismatch";return false;
}
namespace {
struct ProfileAccumulator {
    FrequencyProfile value;
    std::size_t active=0;
    bool has_presence=true,has_peaks=true;
    std::vector<std::size_t> contributors;
    explicit ProfileAccumulator(const ProfileCompatibility& c,const ProfileTraining& t) {
        value.compatibility=c;value.training=t;
        value.mean_spectrum.assign(c.fft_size/2+1,0);value.peak_spectrum.assign(c.fft_size/2+1,0);value.frequency_presence.assign(c.fft_size/2+1,0);
        contributors.assign(c.fft_size/2+1,0);
    }
    void add_spectrum(const CompositionStatistics& track,std::span<const double> source,std::vector<double>& target) {
        for(std::size_t k=0;k<target.size();++k){
            const auto hz=k*value.compatibility.sample_rate/value.compatibility.fft_size;
            if(measured_frequency(track,value.compatibility.sample_rate,hz))
                target[k]+=sample_measured_spectrum(source,measurement_rate(track,value.compatibility.sample_rate),hz);
        }
    }
    bool add_presence(const CompositionStatistics& track,std::string& error) {
        if(track.frequency_presence.size()!=value.frequency_presence.size()){has_presence=false;return true;}
        for(std::size_t k=0;k<track.frequency_presence.size();++k){
            const auto presence=track.frequency_presence[k];
            if(!std::isfinite(presence) || presence<0 || presence>1){error="Invalid composition presence";return false;}
        }
        add_spectrum(track,track.frequency_presence,value.frequency_presence);
        return true;
    }
    bool validate_track(const CompositionStatistics& track,std::string& error) const {
        if(!valid_measurement_grid(track,value.compatibility.sample_rate)){error="Invalid composition frequency grid";return false;}
        if(track.active_frames>track.frames || !std::isfinite(track.rms) || track.rms<0 || track.mean_spectrum.size()!=value.mean_spectrum.size()){
            error="Invalid composition statistics";return false;
        }
        if(!std::all_of(track.mean_spectrum.begin(),track.mean_spectrum.end(),[](double v){return std::isfinite(v) && v>=0;})){error="Invalid spectrum statistics";return false;}
        return true;
    }
    bool add_peaks(const CompositionStatistics& track,std::string& error) {
        if(track.peak_spectrum.size()==value.peak_spectrum.size()){
            for(std::size_t k=0;k<track.peak_spectrum.size();++k){
                const auto peak=track.peak_spectrum[k];
                if(!std::isfinite(peak) || peak<0){error="Invalid composition peak";return false;}
            }
            add_spectrum(track,track.peak_spectrum,value.peak_spectrum);
        }else if(value.training.gain_mode==ProfileGainMode::PeakNormalization){
            error="Peak normalization requires measured per-bin track maxima";return false;
        }else has_peaks=false;
        return true;
    }
    bool add(const CompositionStatistics& track,std::string& error) {
        if(!validate_track(track,error))return false;
        if(!track.active_frames)return true;
        if(!add_presence(track,error) || !add_peaks(track,error))return false;
        ++active;
        add_spectrum(track,track.mean_spectrum,value.mean_spectrum);
        for(std::size_t k=0;k<contributors.size();++k)
            if(measured_frequency(track,value.compatibility.sample_rate,k*value.compatibility.sample_rate/value.compatibility.fft_size))++contributors[k];
        return true;
    }
    bool finish(std::string& error) {
        if(!active){error="No active audio windows above the silence threshold";return false;}
        normalize_spectra();
        if(!has_presence)value.frequency_presence.clear();
        if(!has_peaks)value.peak_spectrum.clear();
        return true;
    }
    void normalize_spectra() {
        for(std::size_t k=0;k<contributors.size();++k)if(contributors[k]){
            value.mean_spectrum[k]/=double(contributors[k]);value.peak_spectrum[k]/=double(contributors[k]);
            if(has_presence)value.frequency_presence[k]/=double(contributors[k]);
        }
    }
    bool collect(std::span<const CompositionStatistics> tracks,std::string& error) {
        for(const auto& track:tracks)if(!add(track,error))return false;
        return finish(error);
    }
};
std::vector<double> smoothed_spectrum(std::span<const double> mean,std::size_t radius) {
    const auto bins=mean.size();
    std::vector<double> smoothed(bins);
    for(std::size_t k=0;k<bins;++k){
        const auto first=k?std::max(std::size_t{1},k>radius?k-radius:0):0;
        const auto last=std::min(bins-1,k+radius);
        smoothed[k]=std::accumulate(mean.begin()+first,mean.begin()+last+1,0.0)/double(last-first+1);
    }
    return smoothed;
}
void build_mean_gains(FrequencyProfile& profile,double floor) {
    const auto& training=profile.training;
    const auto smoothed=smoothed_spectrum(profile.mean_spectrum,training.smoothing_radius);
    const auto bins=smoothed.size();
    const auto target=std::accumulate(smoothed.begin()+1,smoothed.end(),0.0)/double(bins-1);
    profile.gains.resize(bins);
    for(std::size_t k=0;k<bins;++k)
        profile.gains[k]=k && profile.mean_spectrum[k]==0?1.f:float(std::clamp(k && smoothed[k]>floor?target/smoothed[k]:1.,
                                        training.min_gain,training.max_gain));
}
bool build_profile_gains(FrequencyProfile& profile,std::string& error) {
    const auto& c=profile.compatibility;
    const bool amplitude=c.projection.representation==SpectrumProjection::Magnitude ||
                         c.projection.representation==SpectrumProjection::PeakAmplitude;
    const double floor=c.reference*std::pow(10.,c.floor_db/(amplitude?20.:10.));
    if(profile.training.gain_mode!=ProfileGainMode::PeakNormalization){build_mean_gains(profile,floor);return true;}
    if(!build_frequency_peak_compensation_curve(profile.peak_spectrum,floor,profile.training.max_gain,profile.gains)){
        error="Could not normalize measured peak spectrum";return false;
    }
    return true;
}
bool valid_profile_training(const ProfileCompatibility& c,const ProfileTraining& training,std::string& error) {
    if(!valid_profile_settings(c,training,error))return false;
    if(training.gain_mode==ProfileGainMode::PeakNormalization && training.max_gain<1){
        error="Peak normalization requires max_gain >= 1";return false;
    }
    return true;
}
}
bool build_frequency_peak_compensation_curve(std::span<const double> peaks,double floor,
                                             double max_gain,std::vector<float>& output) {
    if(peaks.size()<2 || !std::isfinite(floor) || floor<0 ||
       !std::isfinite(max_gain) || max_gain<1 || max_gain>1000000 ||
       !std::all_of(peaks.begin(),peaks.end(),[](double v){return std::isfinite(v) && v>=0;}))return false;
    std::vector<float> curve(peaks.size(),1.f);
    for(std::size_t k=1;k<peaks.size();++k){
        curve[k]=float(profile_curve_detail::regularized_gain(peaks[k],peaks[k],floor,max_gain));
    }
    output=std::move(curve);
    return true;
}
bool build_frequency_profile(const ProfileCompatibility& c,const ProfileTraining& training,std::span<const CompositionStatistics> tracks,FrequencyProfile& output,std::string& error) {
    if(!valid_profile_training(c,training,error))return false;
    ProfileAccumulator accumulator(c,training);
    if(!accumulator.collect(tracks,error))return false;
    if(!build_profile_gains(accumulator.value,error))return false;
    accumulator.value.compositions.assign(tracks.begin(),tracks.end());
    if(!validate_profile(accumulator.value,c,error))return false;
    output=std::move(accumulator.value);return true;
}
bool PreparedFrequencyNormalizer::prepare(std::span<const float> gains) {
    if(gains.empty() || gains.size()>max_fft_samples/2+1) return false;
    for(float v:gains) if(!signal_contracts::valid_profile_gain(v)) return false;
    gains_.assign(gains.begin(),gains.end());return true;
}
bool PreparedFrequencyNormalizer::prepare_manual(std::size_t bins,float gain) {
    if(!bins||bins>max_fft_samples/2+1||!std::isfinite(gain)||gain<0||gain>1000000) return false;
    gains_.assign(bins,gain); return true;
}
namespace {
bool normalizer_alias_safe(std::span<const float> input,std::span<float> output) noexcept {
    if(input.data()!=output.data()) {
        const auto a=reinterpret_cast<std::uintptr_t>(input.data()), b=reinterpret_cast<std::uintptr_t>(output.data());
        const auto bytes=input.size_bytes(); if(a<b+bytes && b<a+bytes) return false;
    }
    return true;
}
}
bool PreparedFrequencyNormalizer::process(std::span<const float> input,std::span<float> output) const noexcept {
    if(input.size()!=gains_.size()||output.size()!=input.size()||gains_.empty()) return false;
    if(!normalizer_alias_safe(input,output))return false;
    for(std::size_t k=0;k<input.size();++k)
        if(!std::isfinite(input[k])||input[k]<0||double(input[k])*gains_[k]>std::numeric_limits<float>::max()) return false;
    for(std::size_t k=0;k<input.size();++k) output[k]=input[k]*gains_[k];
    return true;
}
}
