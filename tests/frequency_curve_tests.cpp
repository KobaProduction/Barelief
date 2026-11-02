#include <dsp_kernels/frequency_profile.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace dsp_kernels;
ProfileCompatibility frontend() {
    ProfileCompatibility c;c.sample_rate=48000;c.fft_size=4096;c.projection={SpectrumProjection::PeakAmplitude,true};return c;
}
CompositionStatistics track(double scale=1) {
    const auto c=frontend();CompositionStatistics t;t.frames=t.active_frames=100;t.rms=.1;t.measured_max_hz=24000;
    t.mean_spectrum.resize(2049);t.peak_spectrum.resize(2049);
    for(std::size_t k=0;k<t.peak_spectrum.size();++k){
        const auto hz=k*c.sample_rate/c.fft_size;
        t.peak_spectrum[k]=scale*std::exp(-2.-hz/18000.-hz*hz/1e9);t.mean_spectrum[k]=t.peak_spectrum[k]/4;
    }
    return t;
}
FrequencyProfile curve(const FrequencyCurveOptions& o,std::span<const CompositionStatistics> tracks) {
    FrequencyProfile p;std::string error;assert(build_frequency_curve(frontend(),tracks,o,p,error));return p;
}
void aggregation() {
    std::array tracks{track(),track(2),track(100)};FrequencyCurveOptions o;
    o.level_reference=CurveLevelReference::AbsoluteAmplitude;
    auto mean=curve(o,tracks);o.aggregation=PeakAggregation::GlobalMaximum;auto maximum=curve(o,tracks);
    o.aggregation=PeakAggregation::UpperPercentile;o.percentile=90;auto percentile=curve(o,tracks);
    assert(std::abs(mean.peak_spectrum[100]/tracks[0].peak_spectrum[100]-103./3)<1e-12);
    assert(maximum.peak_spectrum[100]==tracks[2].peak_spectrum[100]);
    assert(percentile.peak_spectrum[100]==tracks[1].peak_spectrum[100]);
    assert(percentile.gains[100]>mean.gains[100]);
    tracks[2].active_frames=0;auto without_silence=curve(o,tracks);
    assert(without_silence.peak_spectrum[100]==tracks[0].peak_spectrum[100]);
}
void approximation_quality() {
    const auto input=track();FrequencyCurveOptions o;o.polynomial_degree=2;
    o.level_reference=CurveLevelReference::AbsoluteAmplitude;
    for(const auto method:{CurveApproximation::Polynomial,CurveApproximation::PiecewiseSmooth}){
        o.approximation=method;const auto p=curve(o,std::span(&input,1));
        assert(p.curve->fit_rmse_db<(method==CurveApproximation::Polynomial?1e-6:.02));
        for(double hz=100;hz<23900;hz+=173){
            const auto oracle=std::exp(-2.-hz/18000.-hz*hz/1e9);
            assert(std::abs(20*std::log10(frequency_curve_amplitude(p,hz)/oracle))<.02);
            assert(std::abs(frequency_curve_gain(p,hz+.001)-frequency_curve_gain(p,hz))<.001);
        }
    }
}
void floor_continuity() {
    std::vector<float> gains;const double floor=1e-6;
    std::array<double,5> values{0,floor*(1-1e-7),floor,floor*(1+1e-7),0};
    assert(build_frequency_peak_compensation_curve(values,floor,100,gains));
    assert(gains.front()==1 && gains.back()==1 && gains[2]>1);
    assert(std::abs(gains[1]-gains[3])<.001 && gains[2]<=100);
    auto t=track();std::fill(t.peak_spectrum.begin(),t.peak_spectrum.end(),1e-12);
    FrequencyCurveOptions absolute;absolute.level_reference=CurveLevelReference::AbsoluteAmplitude;
    auto p=curve(absolute,std::span(&t,1));assert(*std::max_element(p.gains.begin(),p.gains.end())<1.001);
    std::fill(t.peak_spectrum.begin(),t.peak_spectrum.end(),1e-6);
    FrequencyCurveOptions capped;capped.max_gain=100;
    capped.level_reference=CurveLevelReference::AbsoluteAmplitude;
    p=curve(capped,std::span(&t,1));
    assert(std::abs(p.gains[100]-50.5)<1e-3);
    std::fill(t.peak_spectrum.begin(),t.peak_spectrum.end(),0);std::string error;const auto before=p.gains;
    assert(!build_frequency_curve(frontend(),std::span(&t,1),{},p,error) && p.gains==before);
}
void rejects_unsupported_grid(const FrequencyProfile& source,const ProfileCompatibility& target) {
    auto output=source;
    std::string error;
    assert(!resample_frequency_profile(source,target,output,error));
    assert(!error.empty());
    assert(profile_mismatches(output,source.compatibility).empty());
    assert(output.gains==source.gains);
    assert(output.mean_spectrum==source.mean_spectrum);
    assert(output.peak_spectrum==source.peak_spectrum);
    assert(output.frequency_presence==source.frequency_presence);
    assert(output.compositions.size()==source.compositions.size());
    assert(output.compositions.front().mean_spectrum==source.compositions.front().mean_spectrum);
    assert(output.compositions.front().peak_spectrum==source.compositions.front().peak_spectrum);
    assert(output.curve.has_value());
    assert(valid_frequency_curve(output));
    assert(output.curve->coefficients==source.curve->coefficients && output.curve->knot_hz==source.curve->knot_hz);
    assert(output.curve->log_amplitude==source.curve->log_amplitude && output.curve->slopes==source.curve->slopes);
}

void grids_and_support() {
    auto t=track();t.measured_max_hz=16000;
    auto p=curve({},std::span(&t,1));
    assert(frequency_curve_gain(p,0)==1 && frequency_curve_gain(p,16001)==1 && frequency_curve_gain(p,25000)==1);
    for(double rate:{32768.,44100.,48000.})for(std::size_t fft:{512,2048,4096,8192}){
        auto c=frontend();c.sample_rate=rate;c.fft_size=fft;FrequencyProfile converted;std::string error;
        if(fft>max_fft_samples){rejects_unsupported_grid(p,c);continue;}
        assert(resample_frequency_profile(p,c,converted,error));
        assert(converted.gains.size()==fft/2+1 && converted.gains[0]==1);
        for(std::size_t k=1;k<converted.gains.size();++k){
            const auto hz=k*rate/fft;
            assert(converted.gains[k]==float(frequency_curve_gain(p,hz)));
            if(hz>p.curve->maximum_hz)assert(converted.gains[k]==1);
        }
    }
    auto unsupported=frontend();unsupported.fft_size=max_fft_samples*2;
    rejects_unsupported_grid(p,unsupported);
}
void logarithmic_knot_resolution(){
    auto calibration=frontend();
    FrequencyMeasurements data;data.edge_hz=calibration.sample_rate/4096;
    for(std::size_t k=0;k<=2048;++k){
        const auto hz=k*calibration.sample_rate/4096;
        const auto bump=hz>0?1.4*std::exp(-std::pow(std::log(hz/45.)/.23,2)):0.;
        const auto amplitude=std::exp(-1.2-hz/8000+bump);
        data.hz.push_back(hz);data.maxima.push_back(amplitude);
        data.mean_amplitude.push_back(amplitude/2);data.contributors.push_back(4);
    }
    FrequencyCurveOptions options;options.segments=64;
    options.approximation=CurveApproximation::PiecewiseSmooth;
    options.knot_spacing=CurveKnotSpacing::LinearHz;
    std::string error;FrequencyProfile old_curve,new_curve;
    assert(fit_frequency_measurements(calibration,data,options,old_curve,error));
    options.knot_spacing=CurveKnotSpacing::LogarithmicHz;
    assert(fit_frequency_measurements(calibration,data,options,new_curve,error));
    const auto hz=data.hz[6];
    const auto measured=20*std::log10(data.maxima[6]);
    const auto error_db=[&](const FrequencyProfile& p){
        return std::abs(20*std::log10(frequency_curve_amplitude(p,hz))-measured);
    };
    assert(error_db(new_curve)<error_db(old_curve));
    assert(error_db(new_curve)<2.);
    for(std::size_t i=1;i<new_curve.curve->knot_hz.size();++i)
        assert(new_curve.curve->knot_hz[i]>new_curve.curve->knot_hz[i-1]);
}
void malformed_and_legacy() {
    auto t=track();auto p=curve({},std::span(&t,1));std::string error;auto invalid=frontend();invalid.window=WindowFunction::Hamming;
    FrequencyProfile output;assert(!resample_frequency_profile(p,invalid,output,error));
    invalid=frontend();invalid.projection.representation=SpectrumProjection::Power;
    assert(!resample_frequency_profile(p,invalid,output,error));
    p.curve->slopes[1]=std::numeric_limits<double>::infinity();assert(!valid_frequency_curve(p));
    p.curve.reset();assert(resample_frequency_profile(p,p.compatibility,output,error));
    invalid=p.compatibility;invalid.sample_rate=44100;assert(!resample_frequency_profile(p,invalid,output,error));
}
void native_measurement_grids() {
    std::array tracks{track(),track(),track()};const std::array rates{32768.,44100.,48000.};
    for(std::size_t i=0;i<tracks.size();++i){
        auto& t=tracks[i];t.sample_rate=t.source_sample_rate=rates[i];t.measured_max_hz=rates[i]/2;
        for(std::size_t k=0;k<t.peak_spectrum.size();++k){
            const auto hz=k*rates[i]/4096;t.peak_spectrum[k]=double(1u<<i)*(.1+hz/1e6);t.mean_spectrum[k]=t.peak_spectrum[k]/4;
        }
    }
    FrequencyCurveOptions absolute;absolute.level_reference=CurveLevelReference::AbsoluteAmplitude;
    const auto p=curve(absolute,tracks);
    for(std::size_t k=1;k<p.gains.size();++k){
        const auto hz=k*48000./4096;double sum=0,count=0;
        for(std::size_t i=0;i<rates.size();++i)if(hz<=rates[i]/2){sum+=double(1u<<i)*(.1+hz/1e6);++count;}
        assert(std::abs(p.peak_spectrum[k]-sum/count)<1e-12);
    }
    FrequencyProfile legacy;std::string error;ProfileTraining training;
    assert(build_frequency_profile(frontend(),training,tracks,legacy,error));
    assert(std::abs(legacy.peak_spectrum[2000]-p.peak_spectrum[2000])<1e-12);
}
void holes_and_boundaries() {
    auto t=track();t.peak_spectrum[100]=0;const auto p=curve({},std::span(&t,1));
    const auto step=p.compatibility.sample_rate/p.compatibility.fft_size;
    assert(frequency_curve_gain(p,100*step)==1);
    for(const auto hz:{p.curve->minimum_hz,p.curve->maximum_hz,100*step}){
        assert(std::abs(frequency_curve_gain(p,hz-1e-8)-frequency_curve_gain(p,hz+1e-8))<.01);
    }
    const auto left=99*step;
    assert(std::abs(frequency_curve_gain(p,left)-frequency_curve_gain(p,left+.00001))<.01);
}
}
int main() {
    aggregation();approximation_quality();floor_continuity();grids_and_support();logarithmic_knot_resolution();malformed_and_legacy();holes_and_boundaries();native_measurement_grids();
    std::cout<<"frequency_curve_tests PASS\n";
}
