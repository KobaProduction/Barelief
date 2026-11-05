#include "curve_analysis.h"
#include <algorithm>
#include <cmath>

namespace desktop_analysis::offline {
using namespace dsp_kernels;
namespace {
CompositionStatistics spectral_track(const CompositionStatistics& t) {
    CompositionStatistics value;value.frames=t.frames;value.active_frames=t.active_frames;value.rms=t.rms;
    value.source_sample_rate=t.source_sample_rate;value.sample_rate=t.sample_rate;value.measured_max_hz=t.measured_max_hz;
    value.mean_spectrum=t.mean_spectrum;value.peak_spectrum=t.peak_spectrum;return value;
}
std::shared_ptr<const FrequencyMeasurements> stored_measurements(const FrequencyProfile& source) {
    if(!source.curve || source.curve->measurement_hz.empty())return {};
    const auto& curve=*source.curve;
    auto saved=std::make_shared<FrequencyMeasurements>();
    saved->hz=curve.measurement_hz;saved->maxima=curve.measured_spectrum;
    saved->mean_amplitude=curve.mean_amplitude;saved->contributors=curve.contributors;
    saved->edge_hz=curve.edge_hz;return saved;
}

bool valid_summary_source(const FrequencyProfile& source,std::span<const double> contributors,std::size_t n) {
    return !(!source.compositions.empty() || source.curve ||
        source.compatibility.fft_size<512 || source.compatibility.fft_size>16384 ||
        (source.compatibility.fft_size&(source.compatibility.fft_size-1)) ||
        source.peak_spectrum.size()!=n || source.mean_spectrum.size()!=n ||
        contributors.size()!=n || !source.compatibility.sample_rate ||
        source.compatibility.projection.representation!=SpectrumProjection::PeakAmplitude ||
        !source.compatibility.projection.compensate);
}
bool append_summary_measurements(const FrequencyProfile& source,std::span<const double> contributors,
                                 std::size_t n,FrequencyMeasurements& values,double& maximum) {
    for(std::size_t k=0;k<n;++k){
        const auto count=contributors[k],peak=source.peak_spectrum[k],mean=source.mean_spectrum[k];
        if(!std::isfinite(count) || count<0 || std::floor(count)!=count ||
           count>10000000 || !std::isfinite(peak) || peak<0 ||
           !std::isfinite(mean) || mean<0 || (!count && (peak>0 || mean>0)))return false;
        values.hz.push_back(k*source.compatibility.sample_rate/double(source.compatibility.fft_size));
        values.contributors.push_back(count);
        values.maxima.push_back(peak);
        values.mean_amplitude.push_back(mean);
        if(k>0 && count>0)maximum=std::max(maximum,peak);
    }
    return true;
}
std::shared_ptr<FrequencyMeasurements> summary_measurements(const FrequencyProfile& source,std::span<const double> contributors,std::size_t n) {
    auto values=std::make_shared<FrequencyMeasurements>();
    double maximum=0;
    if(!append_summary_measurements(source,contributors,n,*values,maximum) || maximum<=0)return {};
    for(auto& value:values->maxima)value/=maximum;
    for(auto& value:values->mean_amplitude)value/=maximum;
    values->edge_hz=source.compatibility.sample_rate/double(source.compatibility.fft_size);
    return values;
}
}
const FrequencyProfile& CurveAnalysis::selected()const {
    return piecewise;
}
std::shared_ptr<const CurveCorpus> curve_corpus(const FrequencyProfile& source,std::span<const std::uint8_t> included) {
    auto corpus=std::make_shared<CurveCorpus>();corpus->calibration=source.compatibility;
    for(std::size_t i=0;i<source.compositions.size();++i){
        if(!included.empty() && (i>=included.size() || !included[i]))continue;
        corpus->tracks.push_back(spectral_track(source.compositions[i]));
    }
    if(corpus->tracks.empty()){
        corpus->saved_measurements=stored_measurements(source);
        if(corpus->saved_measurements)corpus->saved_options=source.curve->options;
    }
    return corpus;
}
std::shared_ptr<const CurveCorpus> curve_corpus_summary(
    const FrequencyProfile& source,std::span<const double> contributors) {
    const auto n=source.compatibility.fft_size/2+1;
    if(!valid_summary_source(source,contributors,n))return {};
    auto corpus=std::make_shared<CurveCorpus>();
    corpus->calibration=source.compatibility;
    auto values=summary_measurements(source,contributors,n);
    if(!values)return {};
    corpus->saved_measurements=std::move(values);
    corpus->saved_options=FrequencyCurveOptions{};
    return corpus;
}
namespace {
bool same_aggregation(const FrequencyCurveOptions& a,const FrequencyCurveOptions& b) {
    return a.level_reference==b.level_reference && a.aggregation==b.aggregation &&
        (a.aggregation!=PeakAggregation::UpperPercentile || a.percentile==b.percentile);
}
bool same_fit(const FrequencyCurveOptions& a,const FrequencyCurveOptions& b) {
    return a.segments==b.segments && a.knot_spacing==b.knot_spacing;
}
bool prepare_measurements(CurveAnalysis& output,const std::shared_ptr<const CurveAnalysis>& previous) {
    if(output.corpus->saved_measurements){
        if(!same_aggregation(output.settings,output.corpus->saved_options)){
            output.error="Saved ready profile has no track spectra: aggregation cannot be changed";
            return false;
        }
        output.measurements=output.corpus->saved_measurements;
        return true;
    }
    if(previous && previous->corpus==output.corpus && same_aggregation(previous->settings,output.settings)){
        output.measurements=previous->measurements;return bool(output.measurements);
    }
    auto measurements=std::make_shared<FrequencyMeasurements>();
    if(!combine_frequency_measurements(output.corpus->calibration,output.corpus->tracks,output.settings,*measurements,output.error))return false;
    output.measurements=std::move(measurements);return true;
}
bool prepare_fit(CurveAnalysis& output,const std::shared_ptr<const CurveAnalysis>& previous) {
    auto& profile=output.piecewise;
    if(previous && previous->measurements==output.measurements && same_fit(previous->settings,output.settings)){
        profile=previous->piecewise;
        if(profile.curve){profile.curve->options=output.settings;return update_frequency_curve_gains(profile,output.piecewise_error);}
    }
    return fit_frequency_measurements(output.corpus->calibration,*output.measurements,
        output.settings,profile,output.piecewise_error);
}
}
CurveAnalysis analyze_frequency_curves(std::shared_ptr<const CurveCorpus> corpus,
    const FrequencyCurveOptions& options,std::shared_ptr<const CurveAnalysis> previous) {
    CurveAnalysis result;result.corpus=std::move(corpus);result.settings=options;
    // New analysis only uses the 64-segment piecewise smooth representation.
    // Legacy polynomial decoding stays in the DSP/storage layer for old files.
    result.settings.approximation=CurveApproximation::PiecewiseSmooth;
    if(!result.corpus){result.error="No scan measurements";return result;}
    if(!prepare_measurements(result,previous))return result;
    if(!prepare_fit(result,previous))result.error=result.piecewise_error;
    return result;
}
}
