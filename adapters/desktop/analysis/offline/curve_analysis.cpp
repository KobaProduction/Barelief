#include "curve_analysis.h"

namespace desktop_analysis::offline {
using namespace dsp_kernels;
namespace {
CompositionStatistics spectral_track(const CompositionStatistics& t) {
    CompositionStatistics value;value.frames=t.frames;value.active_frames=t.active_frames;value.rms=t.rms;
    value.source_sample_rate=t.source_sample_rate;value.sample_rate=t.sample_rate;value.measured_max_hz=t.measured_max_hz;
    value.mean_spectrum=t.mean_spectrum;value.peak_spectrum=t.peak_spectrum;return value;
}
}
const FrequencyProfile& CurveAnalysis::selected()const {
    return settings.approximation==CurveApproximation::Polynomial?polynomial:piecewise;
}
std::shared_ptr<const CurveCorpus> curve_corpus(const FrequencyProfile& source,std::span<const std::uint8_t> included) {
    auto corpus=std::make_shared<CurveCorpus>();corpus->calibration=source.compatibility;
    for(std::size_t i=0;i<source.compositions.size();++i){
        if(!included.empty() && (i>=included.size() || !included[i]))continue;
        corpus->tracks.push_back(spectral_track(source.compositions[i]));
    }
    return corpus;
}
namespace {
bool same_aggregation(const FrequencyCurveOptions& a,const FrequencyCurveOptions& b) {
    return a.aggregation==b.aggregation && (a.aggregation!=PeakAggregation::UpperPercentile || a.percentile==b.percentile);
}
bool same_fit(const FrequencyCurveOptions& a,const FrequencyCurveOptions& b,CurveApproximation method) {
    return method==CurveApproximation::Polynomial?a.polynomial_degree==b.polynomial_degree:a.segments==b.segments;
}
bool prepare_measurements(CurveAnalysis& output,const std::shared_ptr<const CurveAnalysis>& previous) {
    if(previous && previous->corpus==output.corpus && same_aggregation(previous->settings,output.settings)){
        output.measurements=previous->measurements;return bool(output.measurements);
    }
    auto measurements=std::make_shared<FrequencyMeasurements>();
    if(!combine_frequency_measurements(output.corpus->calibration,output.corpus->tracks,output.settings,*measurements,output.error))return false;
    output.measurements=std::move(measurements);return true;
}
bool prepare_fit(CurveAnalysis& output,CurveApproximation method,const std::shared_ptr<const CurveAnalysis>& previous) {
    auto options=output.settings;options.approximation=method;
    auto& profile=method==CurveApproximation::Polynomial?output.polynomial:output.piecewise;
    auto& error=method==CurveApproximation::Polynomial?output.polynomial_error:output.piecewise_error;
    if(previous && previous->measurements==output.measurements && same_fit(previous->settings,options,method)){
        profile=method==CurveApproximation::Polynomial?previous->polynomial:previous->piecewise;
        if(profile.curve){profile.curve->options=options;return update_frequency_curve_gains(profile,error);}
    }
    return fit_frequency_measurements(output.corpus->calibration,*output.measurements,options,profile,error);
}
}
CurveAnalysis analyze_frequency_curves(std::shared_ptr<const CurveCorpus> corpus,
    const FrequencyCurveOptions& options,std::shared_ptr<const CurveAnalysis> previous) {
    CurveAnalysis result;result.corpus=std::move(corpus);result.settings=options;
    if(!result.corpus){result.error="No scan measurements";return result;}
    if(!prepare_measurements(result,previous))return result;
    const auto polynomial_ok=prepare_fit(result,CurveApproximation::Polynomial,previous);
    const auto piecewise_ok=prepare_fit(result,CurveApproximation::PiecewiseSmooth,previous);
    if(options.approximation==CurveApproximation::Polynomial && !polynomial_ok)result.error=result.polynomial_error;
    if(options.approximation==CurveApproximation::PiecewiseSmooth && !piecewise_ok)result.error=result.piecewise_error;
    return result;
}
}
