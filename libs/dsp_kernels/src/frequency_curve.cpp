#include "frequency_curve_math.h"
#include <dsp_kernels/frequency_measurements.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels {
namespace {
bool nonnegative(std::span<const double> values) {
    return std::ranges::all_of(values,[](double x){return std::isfinite(x) && x>=0;});
}
bool finite(std::span<const double> values) {
    return std::ranges::all_of(values,[](double x){return std::isfinite(x);});
}
bool valid_options(const FrequencyCurveOptions& o) {
    return o.aggregation>=PeakAggregation::MeanTrackMaxima && o.aggregation<=PeakAggregation::UpperPercentile &&
        o.approximation>=CurveApproximation::Polynomial && o.approximation<=CurveApproximation::PiecewiseSmooth &&
        std::isfinite(o.percentile) && o.percentile>=50 && o.percentile<=100 &&
        std::isfinite(o.regularization_db) && o.regularization_db>=-300 && o.regularization_db<=0 &&
        std::isfinite(o.max_gain) && o.max_gain>=1 && o.max_gain<=1000000 &&
        o.polynomial_degree>=1 && o.polynomial_degree<=12 && o.segments>=2 && o.segments<=2048;
}
double aggregate(std::vector<double>& values,const FrequencyCurveOptions& o) {
    if(values.empty())return 0;
    if(o.aggregation==PeakAggregation::GlobalMaximum)return *std::max_element(values.begin(),values.end());
    if(o.aggregation==PeakAggregation::UpperPercentile){
        std::sort(values.begin(),values.end());return values[std::size_t(std::floor(o.percentile/100*(values.size()-1)))];
    }
    double sum=0;for(auto value:values)sum+=value;
    return sum/double(values.size());
}
bool valid_track(const CompositionStatistics& t,std::size_t bins,double nyquist) {
    return t.active_frames<=t.frames && t.mean_spectrum.size()==bins && t.peak_spectrum.size()==bins &&
        nonnegative(t.mean_spectrum) && nonnegative(t.peak_spectrum) &&
        valid_measurement_grid(t,2*nyquist);
}
void collect_bin(FrequencyProfile& p,std::span<const CompositionStatistics> tracks,std::size_t k) {
    std::vector<double> values;double mean=0,presence=0;
    const auto hz=k*p.compatibility.sample_rate/p.compatibility.fft_size;
    for(const auto& t:tracks)if(t.active_frames && measured_frequency(t,p.compatibility.sample_rate,hz)){
        const auto rate=measurement_rate(t,p.compatibility.sample_rate);
        values.push_back(sample_measured_spectrum(t.peak_spectrum,rate,hz));mean+=sample_measured_spectrum(t.mean_spectrum,rate,hz);
        if(!p.frequency_presence.empty())presence+=sample_measured_spectrum(t.frequency_presence,rate,hz);
    }
    p.mean_spectrum[k]=values.empty()?0:mean/double(values.size());
    p.peak_spectrum[k]=aggregate(values,p.curve->options);
    if(!p.frequency_presence.empty())p.frequency_presence[k]=values.empty()?0:presence/double(values.size());
}
bool measured_range(FrequencyProfile& p) {
    const auto step=p.compatibility.sample_rate/p.compatibility.fft_size;
    auto& c=*p.curve;c.measured_spectrum=p.peak_spectrum;
    const auto first=std::find_if(c.measured_spectrum.begin()+1,c.measured_spectrum.end(),[](double x){return x>0;});
    const auto last=std::find_if(c.measured_spectrum.rbegin(),c.measured_spectrum.rend(),[](double x){return x>0;});
    if(first==c.measured_spectrum.end())return false;
    c.minimum_hz=double(first-c.measured_spectrum.begin())*step;
    c.maximum_hz=double(c.measured_spectrum.size()-1-(last-c.measured_spectrum.rbegin()))*step;
    return c.maximum_hz>c.minimum_hz;
}
bool collect_measurements(FrequencyProfile& p,std::span<const CompositionStatistics> tracks) {
    const auto bins=p.compatibility.fft_size/2+1;
    p.mean_spectrum.assign(bins,0);p.peak_spectrum.assign(bins,0);
    if(std::ranges::all_of(tracks,[bins](const auto& t){return t.frequency_presence.size()==bins;}))p.frequency_presence.assign(bins,0);
    for(const auto& t:tracks)if(!valid_track(t,bins,p.compatibility.sample_rate/2))return false;
    for(std::size_t k=1;k<bins;++k)collect_bin(p,tracks,k);
    return measured_range(p);
}
bool valid_knots(const FrequencyCurve& c) {
    if(c.knot_hz.size()<2 || c.knot_hz.size()>2050 || c.knot_hz.size()!=c.log_amplitude.size() || c.slopes.size()!=c.knot_hz.size())return false;
    if(!finite(c.knot_hz) || !finite(c.log_amplitude) || !finite(c.slopes))return false;
    for(std::size_t i=1;i<c.knot_hz.size();++i)if(c.knot_hz[i]<=c.knot_hz[i-1])return false;
    return c.knot_hz.front()==c.minimum_hz && c.knot_hz.back()==c.maximum_hz;
}
double interpolated_measurement(const FrequencyProfile& p,double hz) noexcept {
    const auto& values=p.curve->measured_spectrum;
    const auto position=hz*p.compatibility.fft_size/p.compatibility.sample_rate;
    const auto i=std::min(std::size_t(position),values.size()-1),j=std::min(i+1,values.size()-1);
    const auto fraction=position-double(i);
    if(fraction==0)return values[i];
    return values[i]+fraction*(values[j]-values[i]);
}
bool compatible_units(const ProfileCompatibility& a,const ProfileCompatibility& b) {
    const auto a_scale=profile_window_scaling(a).sum/window_sum(a.fft_size,a.window);
    const auto b_scale=profile_window_scaling(b).sum/window_sum(b.fft_size,b.window);
    return a.window==b.window && a.projection==b.projection && a.channel_policy==b.channel_policy &&
        a.channel_index==b.channel_index && a.include_dc==b.include_dc && a.include_nyquist==b.include_nyquist &&
        a.projection.representation==SpectrumProjection::PeakAmplitude && a.projection.compensate && std::abs(a_scale-b_scale)<1e-9;
}
FrequencyProfile curve_candidate(const ProfileCompatibility& frontend,const FrequencyCurveOptions& options) {
    FrequencyProfile p;p.compatibility=frontend;p.training.gain_mode=ProfileGainMode::PeakNormalization;
    p.training.min_gain=1;p.training.max_gain=options.max_gain;p.curve.emplace();p.curve->options=options;return p;
}
void sample_gains(const FrequencyProfile& source,const ProfileCompatibility& target,FrequencyProfile& output) {
    output.gains.assign(target.fft_size/2+1,1);
    for(std::size_t k=1;k<output.gains.size();++k)output.gains[k]=float(frequency_curve_gain(source,k*target.sample_rate/target.fft_size));
}
FrequencyProfile sampled_profile(const FrequencyProfile& source,const ProfileCompatibility& target) {
    FrequencyProfile result;result.compatibility=target;result.training=source.training;
    sample_gains(source,target,result);result.mean_spectrum.assign(result.gains.size(),0);result.peak_spectrum.assign(result.gains.size(),0);
    return result;
}
FrequencyProfile exact_coefficients(const FrequencyProfile& source) {
    FrequencyProfile result;result.compatibility=source.compatibility;result.training=source.training;
    result.gains=source.gains;result.mean_spectrum=source.mean_spectrum;result.peak_spectrum=source.peak_spectrum;
    result.frequency_presence=source.frequency_presence;return result;
}
bool valid_function_samples(const FrequencyProfile& p) {
    if(p.gains.size()!=p.curve->measured_spectrum.size())return false;
    for(std::size_t k=0;k<p.gains.size();++k){
        const auto expected=frequency_curve_gain(p,k*p.compatibility.sample_rate/p.compatibility.fft_size);
        if(!std::isfinite(expected) || !std::isfinite(p.gains[k]) || std::abs(p.gains[k]-expected)>1e-6*std::max(1.,expected))return false;
    }
    return true;
}
}
bool valid_frequency_curve(const FrequencyProfile& p) {
    if(!p.curve)return true;
    const auto& c=*p.curve;
    if(!valid_options(c.options) || !std::isfinite(c.minimum_hz) || !std::isfinite(c.maximum_hz) ||
        c.minimum_hz<=0 || c.maximum_hz<=c.minimum_hz || c.maximum_hz>p.compatibility.sample_rate/2 ||
        !std::isfinite(c.fit_rmse_db) || c.fit_rmse_db<0 ||
        c.measured_spectrum.size()!=p.compatibility.fft_size/2+1 || !nonnegative(c.measured_spectrum))return false;
    if(c.options.max_gain!=p.training.max_gain || p.training.gain_mode!=ProfileGainMode::PeakNormalization ||
        p.compatibility.projection.representation!=SpectrumProjection::PeakAmplitude || !p.compatibility.projection.compensate)return false;
    const auto valid_function=c.options.approximation==CurveApproximation::Polynomial?
        c.coefficients.size()==c.options.polynomial_degree+1 && finite(c.coefficients):valid_knots(c);
    return valid_function && valid_function_samples(p);
}
double frequency_curve_gain(const FrequencyProfile& p,double hz) noexcept {
    if(!p.curve || !std::isfinite(hz) || hz<p.curve->minimum_hz || hz>p.curve->maximum_hz)return 1;
    const auto measured=interpolated_measurement(p,hz);
    const auto fitted=std::exp(std::clamp(profile_curve_detail::fitted_log_amplitude(*p.curve,hz),-700.,700.));
    const auto floor=p.compatibility.reference*std::pow(10.,p.curve->options.regularization_db/20);
    const auto step=p.compatibility.sample_rate/p.compatibility.fft_size;
    const auto edge=std::clamp(std::min(hz-p.curve->minimum_hz,p.curve->maximum_hz-hz)/step,0.,1.);
    const auto gain=profile_curve_detail::regularized_gain(measured,fitted,floor,p.curve->options.max_gain);
    return 1+edge*edge*(3-2*edge)*(gain-1);
}
double frequency_curve_amplitude(const FrequencyProfile& p,double hz) noexcept {
    if(!p.curve || !std::isfinite(hz) || hz<p.curve->minimum_hz || hz>p.curve->maximum_hz)return 0;
    return std::exp(std::clamp(profile_curve_detail::fitted_log_amplitude(*p.curve,hz),-700.,700.));
}
bool build_frequency_curve(const ProfileCompatibility& frontend,std::span<const CompositionStatistics> tracks,
    const FrequencyCurveOptions& options,FrequencyProfile& output,std::string& error) {
    auto p=curve_candidate(frontend,options);
    if(!valid_options(options) || !valid_profile_settings(frontend,p.training,error) ||
        frontend.projection.representation!=SpectrumProjection::PeakAmplitude || !frontend.projection.compensate){error="Curve requires calibrated peak amplitude measurements";return false;}
    if(!collect_measurements(p,tracks) || !profile_curve_detail::fit(*p.curve,frontend.sample_rate/frontend.fft_size)){
        error="Insufficient measured frequencies for approximation";return false;
    }
    p.compositions.assign(tracks.begin(),tracks.end());sample_gains(p,frontend,p);
    if(!valid_frequency_curve(p) || !validate_profile(p,frontend,error))return false;
    output=std::move(p);return true;
}
bool resample_frequency_profile(const FrequencyProfile& p,const ProfileCompatibility& target,
    FrequencyProfile& output,std::string& error) {
    if(!validate_profile(p,p.compatibility,error) || !valid_frequency_curve(p))return false;
    if(!p.curve){if(!validate_profile(p,target,error))return false;output=exact_coefficients(p);return true;}
    if(!compatible_units(p.compatibility,target) || !valid_profile_settings(target,p.training,error)){
        error="Target requires the same calibrated peak amplitude units and channel policy";return false;
    }
    auto result=sampled_profile(p,target);
    if(!validate_profile(result,target,error))return false;
    output=std::move(result);return true;
}
}
