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
    auto p=curve({},std::span(&t,1));assert(*std::max_element(p.gains.begin(),p.gains.end())<1.001);
    std::fill(t.peak_spectrum.begin(),t.peak_spectrum.end(),1e-6);p=curve({},std::span(&t,1));
    assert(std::abs(p.gains[100]-50.5)<1e-3);
    std::fill(t.peak_spectrum.begin(),t.peak_spectrum.end(),0);std::string error;const auto before=p.gains;
    assert(!build_frequency_curve(frontend(),std::span(&t,1),{},p,error) && p.gains==before);
}
void grids_and_support() {
    auto t=track();t.measured_max_hz=16000;
    auto p=curve({},std::span(&t,1));
    assert(frequency_curve_gain(p,0)==1 && frequency_curve_gain(p,16001)==1 && frequency_curve_gain(p,25000)==1);
    for(double rate:{32768.,44100.,48000.})for(std::size_t fft:{512,2048,4096,8192}){
        auto c=frontend();c.sample_rate=rate;c.fft_size=fft;FrequencyProfile converted;std::string error;
        assert(resample_frequency_profile(p,c,converted,error));
        assert(converted.gains.size()==fft/2+1 && converted.gains[0]==1);
        for(std::size_t k=1;k<converted.gains.size();++k){
            const auto hz=k*rate/fft;
            assert(converted.gains[k]==float(frequency_curve_gain(p,hz)));
            if(hz>p.curve->maximum_hz)assert(converted.gains[k]==1);
        }
    }
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
    aggregation();approximation_quality();floor_continuity();grids_and_support();malformed_and_legacy();holes_and_boundaries();
    std::cout<<"frequency_curve_tests PASS\n";
}
