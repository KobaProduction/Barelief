#include <dsp_kernels/frequency_measurements.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels {
namespace {
bool valid_values(std::span<const double> values) {
    return std::ranges::all_of(values,[](double v){return std::isfinite(v) && v>=0;});
}
bool valid_track(const CompositionStatistics& t,double fallback) {
    return valid_measurement_grid(t,fallback) && t.active_frames<=t.frames &&
        t.mean_spectrum.size()>=2 && t.mean_spectrum.size()==t.peak_spectrum.size() &&
        t.mean_spectrum.size()<=max_fft_samples/2+1 &&
        valid_values(t.mean_spectrum) && valid_values(t.peak_spectrum);
}
bool valid_collection(const ProfileCompatibility& frontend,std::span<const CompositionStatistics> tracks,const FrequencyCurveOptions& options) {
    if(tracks.empty() || tracks.size()>4096 || !std::isfinite(options.percentile) || options.percentile<50 || options.percentile>100 ||
        options.aggregation<PeakAggregation::MeanTrackMaxima || options.aggregation>PeakAggregation::UpperPercentile)return false;
    return std::ranges::all_of(tracks,[&](const auto& t){return valid_track(t,frontend.sample_rate);});
}
void append_axis(std::vector<double>& hz,const CompositionStatistics& t,double fallback) {
    if(!t.active_frames)return;
    const auto rate=measurement_rate(t,fallback);
    const auto limit=t.measured_max_hz>0?t.measured_max_hz:rate/2;
    const auto step=rate/(2*double(t.mean_spectrum.size()-1));
    for(std::size_t k=0;k<t.mean_spectrum.size() && k*step<=limit;++k)hz.push_back(k*step);
    hz.push_back(limit);
}
double aggregate(std::vector<double>& values,const FrequencyCurveOptions& options) {
    if(values.empty())return 0;
    if(options.aggregation==PeakAggregation::GlobalMaximum)return *std::max_element(values.begin(),values.end());
    if(options.aggregation==PeakAggregation::UpperPercentile){
        std::sort(values.begin(),values.end());
        return values[std::size_t(std::floor(options.percentile/100*(values.size()-1)))];
    }
    double total=0;for(const auto v:values)total+=v;
    return total/double(values.size());
}
struct Collector {
    const ProfileCompatibility& frontend;
    std::span<const CompositionStatistics> tracks;
    const FrequencyCurveOptions& options;
    FrequencyMeasurements result;
    std::vector<double> values;
    void append(double hz) {
        values.clear();double mean=0;
        for(const auto& t:tracks)if(t.active_frames && measured_frequency(t,frontend.sample_rate,hz)){
            const auto rate=measurement_rate(t,frontend.sample_rate);
            values.push_back(sample_measured_spectrum(t.peak_spectrum,rate,hz));
            mean+=sample_measured_spectrum(t.mean_spectrum,rate,hz);
        }
        result.contributors.push_back(double(values.size()));
        result.mean_amplitude.push_back(values.empty()?0:mean/double(values.size()));
        result.maxima.push_back(aggregate(values,options));
    }
    void axis() {
        for(const auto& t:tracks)append_axis(result.hz,t,frontend.sample_rate);
        std::sort(result.hz.begin(),result.hz.end());
        result.hz.erase(std::unique(result.hz.begin(),result.hz.end()),result.hz.end());
        for(const auto& t:tracks)if(t.active_frames)
            result.edge_hz=std::max(result.edge_hz,measurement_rate(t,frontend.sample_rate)/(2*double(t.mean_spectrum.size()-1)));
    }
};
}
bool combine_frequency_measurements(const ProfileCompatibility& frontend,
    std::span<const CompositionStatistics> tracks,const FrequencyCurveOptions& options,
    FrequencyMeasurements& output,std::string& error) {
    if(!valid_collection(frontend,tracks,options)){
        error="Invalid frequency measurement collection";return false;
    }
    Collector collector{frontend,tracks,options,{}, {}};collector.values.reserve(tracks.size());collector.axis();
    if(collector.result.hz.size()<3 || collector.result.hz.size()>4194304){error="Insufficient or excessive measured frequencies for approximation";return false;}
    for(const auto hz:collector.result.hz)collector.append(hz);
    output=std::move(collector.result);return true;
}
}
