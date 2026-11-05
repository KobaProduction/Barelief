#include "python_export_next.h"
#include "python_export_options.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/metrical_tempogram_sequence.h>
#include <dsp_kernels/local_pulse.h>
#include <array>

namespace originals {
Matrix sidecar(const Request& r,std::string_view suffix,std::size_t rows,std::size_t columns) {
    auto request=r;request.input=r.input.string()+std::string(suffix);request.frames=rows;request.columns=columns;
    return read_input(request);
}
dsp_kernels::EnvelopeTempoConfig recipe_tempo(const Request& r) {
    dsp_kernels::EnvelopeTempoConfig c;c.sample_rate=float(r.number(0));c.hop_samples=r.size(1);
    c.autocorrelation_seconds=double(r.size(3))*double(c.hop_samples)/double(c.sample_rate);
    c.estimator.aggregate=dsp_kernels::TempoAggregation(r.size(4));c.precision=dsp_kernels::NumericalPrecision(r.size(8));return c;
}
dsp_kernels::TempogramSequenceConfig recipe_tempogram(const Request& r) {
    dsp_kernels::TempogramSequenceConfig c;c.frame_rate=float(r.number(0)/r.number(1));
    c.method=dsp_kernels::TempogramMethod(r.size(4));c.autocorrelation.window.length=r.size(3);
    c.autocorrelation.window.precision=dsp_kernels::NumericalPrecision(r.size(8));return c;
}
dsp_kernels::EnvelopeTempoRatiosConfig recipe_ratios(const Request& r) {
    dsp_kernels::EnvelopeTempoRatiosConfig c;c.tempogram=recipe_tempogram(r);
    c.tempogram.method=dsp_kernels::TempogramMethod::Autocorrelation;
    c.ratios.tempo.frame_rate=c.tempogram.frame_rate;
    c.ratios.time_reduction.operation=dsp_kernels::FeatureReduction(r.size(4));return c;
}
dsp_kernels::PcmOnsetStrengthConfig recipe_onset(const Request& r,bool median) {
    dsp_kernels::PcmOnsetStrengthConfig c;c.spectrum=dsp_kernels::mel_power_spectral_parameters(float(r.number(0)),r.size(2),r.size(1));
    c.onset.strength.n_fft=r.size(2);c.onset.strength.hop_length=r.size(1);
    c.spectrum.precision=dsp_kernels::NumericalPrecision(r.size(8));
    c.onset.strength.aggregate=median?dsp_kernels::OnsetAggregate::Median:dsp_kernels::OnsetAggregate::Mean;return c;
}
dsp_kernels::EnvelopeBeatTrackingConfig recipe_beats(const Request& r) {
    dsp_kernels::EnvelopeBeatTrackingConfig c;c.tempo=recipe_tempo(r);c.tempo.estimator.aggregate=dsp_kernels::TempoAggregation::Mean;
    c.tracking.trim=r.size(7)!=0;c.sparse=true;return c;
}
std::optional<std::span<const float>> recipe_bpm(const Request& r,const float& bpm) {
    if(r.size(5))return std::span(&bpm,1);
    return std::nullopt;
}
void recipe_beat_outputs(const dsp_kernels::BeatTrackingSequenceResult& value,Result& result) {
    result.floats("bpm",value.bpm,{value.bpm.size()});result.integers("events",value.frames);
}
namespace {
bool tempo_options(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::TempoEstimatorConfig c;c.frame_rate=50;c.max_tempo=240;c.aggregate=dsp_kernels::TempoAggregation::None;
    const auto prior=sidecar(r,".prior",1,r.columns);dsp_kernels::PreparedTempoEstimator kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,r.columns,c,b);},[&]{return kernel.prepare(r.frames,r.columns,c);}),"prepare supplied-prior tempo options");
    std::optional<PreparedSumReduction> reduction;if(r.size(0))reduction.emplace(r.frames,r.columns);
    std::vector<float> output(r.size(0)?1:r.frames);
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.source(),r.frames,output,prior.values,reduction?reduction->port():dsp_kernels::FloatMatrixReductionPort{});});
    result.floats("values",output,{output.size()});report_workspace(result,kernel.workspace_bytes()+(reduction?reduction->workspace_bytes():0));return ok;
}
dsp_kernels::TempoRatioSequenceConfig ratio_options_config(const Request& r) {
    dsp_kernels::TempoRatioSequenceConfig c;c.tempo.frame_rate=50;c.tempo.max_tempo=240;
    c.dynamic_frequencies=r.size(0)!=0;c.time_reduction.custom=r.size(2)!=0;return c;
}
bool ratio_options(const Request& r,const Matrix& input,Result& result) {
    const auto c=ratio_options_config(r);const auto coordinates=sidecar(r,".frequencies",c.dynamic_frequencies?r.frames:1,r.columns);
    const auto bpm=sidecar(r,".bpm",1,r.frames),prior=sidecar(r,".prior",1,r.columns),factors=sidecar(r,".factors",1,r.size(3));
    dsp_kernels::PreparedTempoRatioSequence kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,r.columns,c,factors.values.size(),b);},
        [&]{return kernel.prepare(r.frames,r.columns,c,c.dynamic_frequencies?std::span<const float>{}:std::span(coordinates.values),std::span<const float>(factors.values));}),"prepare dynamic ratio options");
    std::optional<PreparedSumReduction> reduction;if(c.time_reduction.custom)reduction.emplace(r.frames,kernel.factors());
    Matrix output(kernel.output_frames(r.frames),kernel.factors());
    const auto supplied=r.size(1)?std::optional{std::span<const float>(bpm.values)}:std::nullopt;
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.source(),r.frames,output.destination(),supplied,prior.values,
        c.dynamic_frequencies?coordinates.source():dsp_kernels::FloatMatrixSource{},reduction?reduction->port():dsp_kernels::FloatMatrixReductionPort{});});
    result.floats("values",output.values,{output.values.size()/output.columns,output.columns});
    report_workspace(result,kernel.workspace_bytes()+(reduction?reduction->workspace_bytes():0));return ok;
}
bool beat_options(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::EnvelopeBeatTrackingConfig c;c.tempo.sample_rate=50;c.tempo.hop_samples=1;c.tempo.autocorrelation_seconds=1.28;
    c.tracking.trim=r.size(0)!=0;const auto bpm=sidecar(r,".bpm",1,r.frames);dsp_kernels::PreparedEnvelopeBeatTracking kernel;
    require(kernel.prepare(r.frames,c),"prepare full per-frame BPM beat options");dsp_kernels::BeatTrackingSequenceResult output;
    const bool ok=kernel.process(input.values,output,std::span<const float>(bpm.values));recipe_beat_outputs(output,result);
    report_workspace(result,kernel.workspace_bytes());return ok;
}
bool pulse_options(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::LocalPulseConfig c;c.window.length=64;c.frame_rate=100;c.min_bpm=30;c.max_bpm=300;
    const auto prior=sidecar(r,".prior",r.size(1)?r.frames:1,r.columns/2);dsp_kernels::PreparedLocalPulse kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.size(0),c,{},b);},[&]{return kernel.prepare(r.size(0),c);}),"prepare explicit Fourier/prior local pulse options");std::vector<float> output(r.size(0));
    const auto static_prior=r.size(1)?std::span<const float>{}:std::span(prior.values);
    const auto prior_rows=r.size(1)?prior.source():dsp_kernels::FloatMatrixSource{};
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.source(),r.frames,output,static_prior,prior_rows);});
    result.floats("values",output,{output.size()});report_workspace(result,kernel.workspace_bytes());return ok;
}
bool metrical(const Request& r,const Matrix& input,Result& result) {
    const auto coordinates=sidecar(r,".frequencies",1,r.columns);const auto factors=sidecar(r,".factors",1,r.size(1));
    dsp_kernels::MetricalTempogramSequenceConfig c;c.tempo_reduction.operation=dsp_kernels::FeatureReduction(r.size(0));
    dsp_kernels::PreparedMetricalTempogramSequence kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.columns,c,factors.values.size(),b);},[&]{return kernel.prepare(coordinates.values,c,std::span<const float>(factors.values));}),"prepare metrogram");
    Matrix output(r.frames,kernel.columns());const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.source(),r.frames,output.destination());});
    result.floats("values",output.values,{r.frames,output.columns});return ok;
}
bool envelope_tempo(const Request& r,const Matrix& input,Result& result) {
    const auto c=recipe_tempo(r);dsp_kernels::PreparedEnvelopeTempo kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,c,b);},[&]{return kernel.prepare(r.frames,c);}),"prepare full envelope tempo");
    std::vector<float> output(kernel.output_frames(r.frames));const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,output);});
    result.floats("values",output,{output.size()});return ok;
}
bool envelope_ratios(const Request& r,const Matrix& input,Result& result) {
    constexpr std::array<float,5> factors{.5f,1,2,3,4};const float bpm=float(r.number(6));
    const auto c=recipe_ratios(r);dsp_kernels::PreparedEnvelopeTempoRatios kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,c,{},{},std::span<const float>(factors),b);},
        [&]{return kernel.prepare(r.frames,c,{},{},std::span<const float>(factors));}),"prepare full envelope ratios");
    Matrix output(kernel.output_frames(r.frames),kernel.factors());
    const auto supplied=recipe_bpm(r,bpm);
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,output.destination(),supplied);});
    result.floats("values",output.values,{output.values.size()/output.columns,output.columns});return ok;
}
bool envelope_beats(const Request& r,const Matrix& input,Result& result) {
    const float bpm=float(r.number(6));const auto c=recipe_beats(r);dsp_kernels::PreparedEnvelopeBeatTracking kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,c,b);},[&]{return kernel.prepare(r.frames,c);}),"prepare full envelope beats");
    const auto supplied=recipe_bpm(r,bpm);dsp_kernels::BeatTrackingSequenceResult output;
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,output,supplied);});
    recipe_beat_outputs(output,result);return ok;
}
}
namespace {
bool composition_options(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="tempo_options")return tempo_options(r,input,result);
    if(r.method=="ratio_options")return ratio_options(r,input,result);
    if(r.method=="beat_options")return beat_options(r,input,result);
    if(r.method=="pulse_options")return pulse_options(r,input,result);
    return pcm_compositions(r,input,result);
}
}

bool compositions(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="metrical")return metrical(r,input,result);
    if(r.method=="envelope_tempo")return envelope_tempo(r,input,result);
    if(r.method=="envelope_ratios")return envelope_ratios(r,input,result);
    if(r.method=="envelope_beats")return envelope_beats(r,input,result);
    return composition_options(r,input,result);
}
}
