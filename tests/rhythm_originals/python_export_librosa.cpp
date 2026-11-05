#include "python_export.h"
#include "python_export_options.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/pcm_onset_envelope.h>
#include <dsp_kernels/onset_strength.h>
#include <dsp_kernels/onset_detection_sequence.h>
#include <dsp_kernels/tempogram_sequence.h>
#include <dsp_kernels/tempo_estimator.h>
#include <dsp_kernels/beat_tracker.h>
#include <dsp_kernels/envelope_local_pulse.h>
#include <dsp_kernels/pcm_onset_strength.h>
#include <dsp_kernels/tempo_ratio_sequence.h>
#include <dsp_kernels/metrical_tempogram_sequence.h>
#include <algorithm>
#include <array>

namespace originals {
namespace {
struct OnsetGroups {
    std::vector<std::size_t> first,second;
    explicit OnsetGroups(bool slices):first(slices?std::vector<std::size_t>{0,1,2,3}:std::vector<std::size_t>{0,1,2}),
        second(slices?std::vector<std::size_t>{2,3,4,5,6,7}:std::vector<std::size_t>{3,4,5,6,7}){}
    std::array<dsp_kernels::OnsetFeatureGroup,2> view() const noexcept {return {dsp_kernels::OnsetFeatureGroup{first},dsp_kernels::OnsetFeatureGroup{second}};}
};
dsp_kernels::OnsetStrengthSequenceConfig strength_options_config(const Request& r) {
    dsp_kernels::OnsetStrengthSequenceConfig c;c.strength.lag=2;c.strength.max_size=3;c.strength.n_fft=64;c.strength.hop_length=16;
    c.strength.detrend=r.size(3)!=0;c.aggregate_features=r.size(1)!=3;
    c.strength.aggregate=dsp_kernels::OnsetAggregate(std::min(r.size(1),std::size_t{2}));return c;
}
bool strength_options(const Request& r,const Matrix& input,Result& result) {
    const auto c=strength_options_config(r);OnsetGroups indices(r.size(0)!=0);const auto groups=indices.view();
    OnsetSumReduction custom(r.columns);dsp_kernels::PreparedOnsetStrengthSequence kernel;
    const auto aggregation=r.size(1)==4?custom.port():dsp_kernels::OnsetAggregationPort{};
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.columns,r.frames,c,groups,b);},
        [&]{return kernel.prepare(r.columns,r.frames,c,groups,aggregation);}),"prepare grouped onset options");
    const auto reference=r.size(2)?sidecar(r,".reference",r.frames,r.columns):Matrix(0,r.columns);
    const auto reference_view=r.size(2)?reference.source():dsp_kernels::FloatMatrixSource{};
    Matrix output(kernel.output_frames(r.frames),kernel.output_features());
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.source(),r.frames,output.destination(),reference_view);});
    result.floats("values",output.values,{output.values.size()/output.columns,output.columns});
    report_workspace(result,kernel.workspace_bytes()+(r.size(1)==4?custom.workspace_bytes():0));return ok;
}
dsp_kernels::OnsetDetectionSequenceConfig detection_options_config(const Request& r) {
    dsp_kernels::OnsetDetectionSequenceConfig c;c.sample_rate=8000;c.hop_samples=80;c.sparse=r.size(0)!=0;c.units=dsp_kernels::EventCoordinateUnit(r.size(1));
    if(r.size(2)){
        dsp_kernels::OnsetPeakConfig p;p.pre_max=2;p.post_max=2;p.pre_average=3;p.post_average=4;
        p.wait=c.sparse?2:0;p.delta=c.sparse?.05f:0;p.normalize=true;c.peaks=p;
    }
    return c;
}
struct DetectionChannels {
    std::vector<std::vector<float>> data;
    std::vector<std::span<const float>> views;
    explicit DetectionChannels(const Matrix& input):data(input.columns,std::vector<float>(input.values.size()/input.columns)) {
        for(std::size_t row=0;row<data.front().size();++row)
            for(std::size_t column=0;column<data.size();++column)data[column][row]=input.values[row*data.size()+column];
        for(const auto& values:data)views.emplace_back(values);
    }
};
struct DetectionMask {
    std::size_t channels;
    std::vector<std::uint8_t> values;
    DetectionMask(std::size_t frames,std::size_t count):channels(count),values(frames*count){}
    static bool write(void* context,std::size_t channel,const dsp_kernels::OnsetDetectionResult& output) noexcept {
        auto& self=*static_cast<DetectionMask*>(context);
        if(channel>=self.channels || output.dense.size()!=self.values.size()/self.channels)return false;
        for(std::size_t row=0;row<output.dense.size();++row)self.values[row*self.channels+channel]=output.dense[row];return true;
    }
};
bool detection_dense(const Request& r,const Matrix& input,dsp_kernels::PreparedOnsetDetectionSequence& kernel,Result& result) {
    DetectionChannels channels(input);DetectionMask output(r.frames,r.columns);
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process_channels(channels.views,{&output,DetectionMask::write});});
    result.mask("mask",output.values);return ok;
}
bool detection_sparse(const Request& r,const Matrix& input,dsp_kernels::PreparedOnsetDetectionSequence& kernel,Result& result) {
    dsp_kernels::OnsetDetectionResult output;const bool ok=kernel.process(input.values,output);
    if(r.size(1)==2)result.doubles("events",output.seconds,{output.seconds.size()});
    else if(r.size(1)==1){const std::vector<std::size_t> samples(output.samples.begin(),output.samples.end());result.integers("events",samples);}
    else result.integers("events",output.frames);return ok;
}
bool detect_options(const Request& r,const Matrix& input,Result& result) {
    const auto c=detection_options_config(r);dsp_kernels::PreparedOnsetDetectionSequence kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,c,0,b);},[&]{return kernel.prepare(r.frames,c);}),"prepare detection options");
    const bool ok=r.size(0)?detection_sparse(r,input,kernel,result):detection_dense(r,input,kernel,result);
    report_workspace(result,kernel.workspace_bytes());return ok;
}
bool strength(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::OnsetStrengthConfig c;c.lag=r.size(0);c.max_size=r.size(1);c.center=r.size(2)!=0;c.detrend=r.size(3)!=0;
    c.aggregate=dsp_kernels::OnsetAggregate(r.size(4));c.n_fft=r.size(5);c.hop_length=r.size(6);
    dsp_kernels::PreparedOnsetStrength kernel;require(kernel.prepare(r.columns,std::max(r.frames,std::size_t{1}),c),"prepare onset strength");
    std::vector<float> output(kernel.output_frames(r.frames));const bool ok=kernel.process(input.source(),r.frames,output);
    result.floats("values",output,{output.size()});return ok;
}
bool detect(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::OnsetDetectionSequenceConfig c;c.sample_rate=float(r.number(0));c.hop_samples=r.size(1);c.backtrack=r.size(2)!=0;
    dsp_kernels::PreparedOnsetDetectionSequence kernel;require(kernel.prepare(std::max(r.frames,std::size_t{1}),c,r.frames),"prepare onset detection");
    dsp_kernels::OnsetDetectionResult output;const bool ok=kernel.process(input.values,output,c.backtrack?std::optional{std::span<const float>(input.values)}:std::nullopt);
    result.integers("events",output.frames);result.mask("mask",output.dense);result.doubles("seconds",output.seconds,{output.seconds.size()});return ok;
}
dsp_kernels::TempogramSequenceConfig temporal_config(const Request& r) {
    dsp_kernels::TempogramSequenceConfig c;c.method=dsp_kernels::TempogramMethod(r.size(0));c.frame_rate=float(r.number(1));
    c.autocorrelation.window.length=r.size(2);c.autocorrelation.window.hop=r.size(3);c.autocorrelation.window.center=r.size(4)!=0;
    c.autocorrelation.window.periodic=true;c.autocorrelation.normalization=dsp_kernels::VectorNormalization(r.size(5));return c;
}
struct TemporalTrace {
    dsp_kernels::PreparedTempogramSequence* kernel;
    Matrix output,acf,fourier;
    TemporalTrace(dsp_kernels::PreparedTempogramSequence& k,std::size_t frames,std::size_t length):kernel(&k),output(frames,k.columns()),acf(frames,length),fourier(frames,2*(length/2+1)){}
    static bool write(void* context,std::size_t row,std::span<const float> values) noexcept {
        auto& self=*static_cast<TemporalTrace*>(context);const auto view=self.kernel->view();
        return Matrix::write(&self.output,row,values) && Matrix::write(&self.acf,row,view.autocorrelation.normalized_values) &&
            Matrix::write(&self.fourier,row,view.fourier.complex_values);
    }
    void emit(Result& result,std::size_t frames) const {
        result.floats("values",output.values,{frames,output.columns});result.floats("acf_values",acf.values,{frames,acf.columns});
        result.floats("fourier_values",fourier.values,{frames,fourier.columns});
    }
};
bool tempogram(const Request& r,const Matrix& input,Result& result) {
    auto config=temporal_config(r);config.autocorrelation.window.precision=dsp_kernels::NumericalPrecision(r.size(6));
    dsp_kernels::PreparedTempogramSequence kernel;require(kernel.prepare(config),"prepare tempogram");report_workspace(result,kernel.workspace_bytes());
    const auto frames=kernel.frames(input.values.size());
    if(r.size(0)==2){
        TemporalTrace trace(kernel,frames,r.size(2));const bool ok=kernel.process(input.values,{&trace,TemporalTrace::write});
        trace.emit(result,frames);result.floats("coordinates",kernel.coordinates_bpm(),{kernel.coordinates_bpm().size()});return ok;
    }
    Matrix output(frames,kernel.columns());
    const bool ok=kernel.process(input.values,output.destination());result.floats("values",output.values,{frames,output.columns});
    result.floats("coordinates",kernel.coordinates_bpm(),{kernel.coordinates_bpm().size()});return ok;
}
bool tempo(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::TempoEstimatorConfig c;c.frame_rate=float(r.number(0));c.start_bpm=float(r.number(1));c.std_bpm=float(r.number(2));
    c.max_tempo=float(r.number(3));c.aggregate=dsp_kernels::TempoAggregation(r.size(4));
    dsp_kernels::PreparedTempoEstimator kernel;require(kernel.prepare(r.frames,r.columns,c),"prepare tempo estimator");
    std::vector<float> output(c.aggregate==dsp_kernels::TempoAggregation::None?r.frames:1);
    const bool ok=kernel.process(input.source(),r.frames,output);result.floats("values",output,{output.size()});return ok;
}
bool beat(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::BeatTrackerConfig c;c.frame_rate=float(r.number(0));c.tightness=float(r.number(2));c.trim=r.size(3)!=0;
    dsp_kernels::PreparedBeatTracker kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,c,b);},[&]{return kernel.prepare(r.frames,c);}),"prepare beat tracker");
    const float bpm=float(r.number(1));std::vector<std::uint8_t> mask(r.frames);
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,std::span(&bpm,1),mask);});
    result.mask("mask",mask);result.floats("local",kernel.local_scores(),{kernel.local_scores().size()});
    result.floats("cumulative",kernel.cumulative_scores(),{kernel.cumulative_scores().size()});result.signed_integers("backlinks",kernel.backlinks());return ok;
}
void pcm_diagnostics(const Matrix& input,dsp_kernels::PcmOnsetStrengthConfig c,Result& result) {
    auto features=spectral_stages(input.values,c.spectrum,result);Matrix db(features.values.size()/features.columns,features.columns);
    dsp_kernels::PreparedPowerToDb decibels;require(decibels.prepare(db.values.size()/db.columns,db.columns,c.decibels),"prepare diagnostic dB");
    require(decibels.process(features.source(),db.values.size()/db.columns,db.columns,db.destination()),"diagnostic dB");
    result.floats("db",db.values,{db.values.size()/db.columns,db.columns});
}
bool pcm_envelope(const Request& r,const Matrix& input,dsp_kernels::PcmOnsetStrengthConfig c,Result& result) {
    dsp_kernels::PreparedPcmOnsetEnvelope kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(input.values.size(),c,{},{},b);},
        [&]{return kernel.prepare(input.values.size(),c);}),"prepare actual PCM onset envelope");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values);});
    result.floats("values",kernel.values(),{kernel.values().size(),1});report_workspace(result,kernel.workspace_bytes());
    pcm_diagnostics(input,c,result);return ok;
}
bool pcm_strength(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::PcmOnsetStrengthConfig c;c.spectrum=dsp_kernels::mel_power_spectral_parameters(float(r.number(0)),r.size(1),r.size(2));
    c.spectrum.precision=dsp_kernels::NumericalPrecision(r.size(6));
    c.onset.strength.n_fft=r.size(1);c.onset.strength.hop_length=r.size(2);
    c.onset.strength.aggregate=dsp_kernels::OnsetAggregate(r.size(3));c.onset.strength.max_size=r.size(4);c.onset.strength.detrend=r.size(5)!=0;
    if(benchmark::enabled())return pcm_envelope(r,input,c,result);
    dsp_kernels::PreparedPcmOnsetStrength kernel;require(kernel.prepare(input.values.size(),c),"prepare PCM onset strength");
    report_workspace(result,kernel.workspace_bytes());
    Matrix output(kernel.frames(input.values.size()),kernel.output_features());const bool ok=kernel.process(input.values,output.destination());
    result.floats("values",output.values,{output.values.size()/output.columns,output.columns});pcm_diagnostics(input,c,result);return ok;
}
bool pulse(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::EnvelopeLocalPulseConfig c;c.sample_rate=float(r.number(0));c.hop_samples=r.size(1);
    c.pulse.frame_rate=c.sample_rate/float(c.hop_samples);c.pulse.window.length=r.size(2);
    c.pulse.min_bpm=float(r.number(3));c.pulse.max_bpm=float(r.number(4));
    dsp_kernels::PreparedEnvelopeLocalPulse kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,c,{},b);},[&]{return kernel.prepare(r.frames,c);}),"prepare PLP");
    std::vector<float> output(r.frames);const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,output);});
    result.floats("values",output,{output.size()});return ok;
}
bool ratios(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::TempoRatioSequenceConfig c;c.tempo.frame_rate=float(r.number(0));c.time_reduction.operation=dsp_kernels::FeatureReduction(r.size(2));
    const std::array<float,5> factors{.5f,1.f,2.f,3.f,4.f};const float bpm=float(r.number(1));
    dsp_kernels::PreparedTempoRatioSequence kernel;require(kernel.prepare(r.frames,r.columns,c,{},std::span<const float>(factors)),"prepare tempogram ratios");
    Matrix output(kernel.output_frames(r.frames),kernel.factors());
    const bool ok=kernel.process(input.source(),r.frames,output.destination(),std::span(&bpm,1));
    result.floats("values",output.values,{output.values.size()/output.columns,output.columns});return ok;
}
}
namespace {
bool librosa_onsets(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="strength_options")return strength_options(r,input,result);
    if(r.method=="detect_options")return detect_options(r,input,result);
    if(r.method=="strength")return strength(r,input,result);
    if(r.method=="detect")return detect(r,input,result);
    if(r.method=="pcm_strength")return pcm_strength(r,input,result);
    require(false,"unknown librosa method");return false;
}
}

bool librosa(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="tempogram")return tempogram(r,input,result);
    if(r.method=="tempo")return tempo(r,input,result);
    if(r.method=="beat")return beat(r,input,result);
    if(r.method=="pulse")return pulse(r,input,result);
    if(r.method=="ratios")return ratios(r,input,result);
    return librosa_onsets(r,input,result);
}
}
