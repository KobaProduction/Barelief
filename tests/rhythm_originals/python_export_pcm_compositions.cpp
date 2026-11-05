#include "python_export_next.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/pcm_tempo.h>
#include <dsp_kernels/pcm_beat_tracking.h>
#include <array>

namespace originals {
namespace {
void onset_output(std::span<const float> values,Result& result) {
    result.floats("onset",values,{values.size()});
}
bool tempo(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::PcmTempoConfig c;c.onset=recipe_onset(r);c.tempo=recipe_tempo(r);
    dsp_kernels::PreparedPcmTempo kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(input.values.size(),c,{},b);},[&]{return kernel.prepare(input.values.size(),c);}),"prepare full PCM tempo");
    report_workspace(result,kernel.workspace_bytes());
    std::vector<float> output(kernel.output_frames(input.values.size()));const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,output);});
    result.floats("values",output,{output.size()});onset_output(kernel.onset_envelope(),result);return ok;
}
bool tempogram(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::PcmTempogramConfig c;c.onset=recipe_onset(r);c.tempogram=recipe_tempogram(r);
    std::size_t onset_frames=0;require(dsp_kernels::PreparedPcmOnsetEnvelope::geometry(input.values.size(),c.onset,{},onset_frames),"PCM onset geometry");
    dsp_kernels::PreparedPcmTempogram kernel;std::size_t bound=0;
    require(decltype(kernel)::storage_bytes(input.values.size(),c,{},{},bound),"query full PCM tempogram cold bound");
    require(benchmark::measured_prepare(bound,[&]{return kernel.prepare(input.values.size(),c);}),"prepare full PCM tempogram");
    report_workspace(result,kernel.workspace_bytes());
    dsp_kernels::PreparedTempogramSequence geometry;require(geometry.prepare(c.tempogram),"temporal geometry");
    Matrix output(geometry.frames(onset_frames),kernel.columns());
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,output.destination());});
    result.floats("values",output.values,{output.values.size()/output.columns,output.columns});onset_output(kernel.onset_envelope(),result);
    result.floats("coordinates",kernel.coordinates_bpm(),{kernel.coordinates_bpm().size()});return ok;
}
bool ratios(const Request& r,const Matrix& input,Result& result) {
    constexpr std::array<float,5> factors{.5f,1,2,3,4};const float bpm=float(r.number(6));
    dsp_kernels::PcmTempoRatiosConfig c;c.onset=recipe_onset(r);c.ratios=recipe_ratios(r);
    dsp_kernels::PreparedPcmTempoRatios kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(input.values.size(),c,{},{},{},std::span<const float>(factors),b);},
        [&]{return kernel.prepare(input.values.size(),c,{},{},{},std::span<const float>(factors));}),"prepare full PCM ratios");
    report_workspace(result,kernel.workspace_bytes());
    Matrix output(kernel.output_frames(input.values.size()),kernel.factors());
    const auto supplied=recipe_bpm(r,bpm);
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,output.destination(),supplied);});
    result.floats("values",output.values,{output.values.size()/output.columns,output.columns});onset_output(kernel.onset_envelope(),result);return ok;
}
bool beats(const Request& r,const Matrix& input,Result& result) {
    const float bpm=float(r.number(6));dsp_kernels::PcmBeatTrackingConfig c;c.onset=recipe_onset(r,true);c.beats=recipe_beats(r);
    dsp_kernels::PreparedPcmBeatTracking kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(input.values.size(),c,{},b);},[&]{return kernel.prepare(input.values.size(),c);}),"prepare full PCM beats");
    report_workspace(result,kernel.workspace_bytes());
    const auto supplied=recipe_bpm(r,bpm);dsp_kernels::BeatTrackingSequenceResult output;
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,output,supplied);});
    recipe_beat_outputs(output,result);onset_output(kernel.onset_envelope(),result);return ok;
}
bool pulse(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::PcmLocalPulseConfig c;c.onset=recipe_onset(r,true);c.pulse.sample_rate=float(r.number(0));c.pulse.hop_samples=r.size(1);
    c.pulse.pulse.frame_rate=c.pulse.sample_rate/float(c.pulse.hop_samples);c.pulse.pulse.window.length=r.size(3);
    c.pulse.pulse.min_bpm=30;c.pulse.pulse.max_bpm=300;
    c.pulse.pulse.window.precision=dsp_kernels::NumericalPrecision(r.size(8));
    dsp_kernels::PreparedPcmLocalPulse kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(input.values.size(),c,{},{},b);},[&]{return kernel.prepare(input.values.size(),c);}),"prepare full PCM PLP");
    report_workspace(result,kernel.workspace_bytes());
    std::vector<float> output(kernel.output_frames(input.values.size()));const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,output);});
    result.floats("values",output,{output.size()});onset_output(kernel.onset_envelope(),result);return ok;
}
}
bool pcm_compositions(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="pcm_tempo")return tempo(r,input,result);
    if(r.method=="pcm_tempogram")return tempogram(r,input,result);
    if(r.method=="pcm_ratios")return ratios(r,input,result);
    if(r.method=="pcm_beats")return beats(r,input,result);
    if(r.method=="pcm_pulse")return pulse(r,input,result);
    require(false,"unknown composition method");return false;
}
}
