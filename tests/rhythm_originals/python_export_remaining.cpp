#include "python_export_remaining.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/onset_peaks.h>
#include <dsp_kernels/local_pulse.h>
#include <dsp_kernels/beat_feature_synchronization.h>
#include <dsp_kernels/model_prediction_selection.h>
#include <dsp_kernels/tempo_probability_histogram.h>
#include <dsp_kernels/complex_values.h>
#include <cmath>

namespace originals {
namespace {
bool backtrack(const Request& r,const Matrix& input,Result& result) {
    const auto events=sidecar(r,".events",1,r.size(0));std::vector<std::size_t> indices;
    for(float value:events.values){require(value>=0 && value==std::floor(value) && value<float(r.frames),"explicit event index");indices.push_back(std::size_t(value));}
    dsp_kernels::PreparedOnsetBacktracker kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,b);},[&]{return kernel.prepare(r.frames);}),"prepare standalone backtrack");
    std::vector<std::size_t> output(indices.size());const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(indices,input.values,output);});
    result.integers("events",output);report_workspace(result,kernel.workspace_bytes());return ok;
}
bool local_pulse(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::LocalPulseConfig c;c.window.length=r.size(1);c.frame_rate=float(r.number(2));c.min_bpm=30;c.max_bpm=300;
    dsp_kernels::PreparedLocalPulse kernel;require(kernel.prepare(r.size(0),c),"prepare direct local pulse");
    std::vector<float> output(r.size(0));const bool ok=kernel.process(input.source(),r.frames,output);
    result.floats("values",output,{output.size()});report_workspace(result,kernel.workspace_bytes());return ok;
}
bool synchronization(const Request& r,const Matrix& input,Result& result) {
    const auto beats=sidecar(r,".beats",1,r.size(1));dsp_kernels::BeatFeatureSynchronizationConfig c;
    require(beats.values.size()>=2,"explicit synchronization beat count");
    c.subdivisions=r.size(0);c.frame_rate=float(r.number(2));dsp_kernels::PreparedBeatFeatureSynchronization kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.columns,c,b);},[&]{return kernel.prepare(r.columns,c);}),"prepare synchronization");Matrix output((beats.values.size()-1)*c.subdivisions,r.columns);
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.source(),r.frames,r.columns,beats.values,output.destination());});
    const auto rows=kernel.retained_beats()?(kernel.retained_beats()-1)*c.subdivisions:0;
    result.floats("values",std::span(output.values).first(rows*r.columns),{rows,r.columns});report_workspace(result,kernel.workspace_bytes());return ok;
}
bool selection(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::PreparedModelPredictionSelection kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,r.columns,b);},[&]{return kernel.prepare(r.frames,r.columns);}),"prepare prediction selection");
    const auto references=r.size(0);
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.source(),r.frames,r.columns,references);});
    result.floats("values",kernel.selected_values(),{kernel.selected_values().size()});
    result.floats("reference",kernel.reference(),{kernel.reference().size()});
    result.floats("errors",kernel.candidate_errors(),{kernel.candidate_errors().size()});
    const auto selected=kernel.selected_model();result.integers("selected",selected==SIZE_MAX?std::span<const std::size_t>{}:std::span(&selected,1));
    report_workspace(result,kernel.workspace_bytes());return ok;
}
bool probabilities(const Request& r,const Matrix& input,Result& result) {
    const auto minimum=float(r.number(0)),maximum=float(r.number(1));
    dsp_kernels::TempoProbabilityRange range;require(dsp_kernels::tempo_probability_range(input.values,minimum,maximum,range),"tempo probability range");
    std::vector<float> coordinates(range.count);require(benchmark::measured_prepare(0,[]{return true;}),"stateless probability recipe has no owned prepare storage");
    const bool ok=benchmark::measured_process(r.prefix,0,[&]{return dsp_kernels::tempo_probability_range(input.values,minimum,maximum,range) && dsp_kernels::tempo_probability_coordinates(range,coordinates);});
    result.floats("values",std::span(input.values).subspan(range.first,range.count),{range.count});
    result.floats("coordinates",coordinates,{range.count});return ok;
}
bool polar(const Request& r,const Matrix& input,Result& result) {
    require(r.columns==2,"explicit real/imaginary columns");std::vector<float> magnitude(r.frames);
    const bool ok=dsp_kernels::cartesian_to_polar(input.values,magnitude,{},dsp_kernels::ZeroMagnitudePhase::Atan2,dsp_kernels::ComplexMagnitudeNorm(r.size(0)));
    result.floats("magnitude",magnitude,{r.frames});return ok;
}
}
namespace {
bool remaining_vectors(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="selection")return selection(r,input,result);
    if(r.method=="probabilities")return probabilities(r,input,result);
    if(r.method=="polar")return polar(r,input,result);
    return remaining_pcm(r,input,result);
}
}

bool remaining(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="backtrack")return backtrack(r,input,result);
    if(r.method=="local_pulse")return local_pulse(r,input,result);
    if(r.method=="synchronization")return synchronization(r,input,result);
    if(r.method=="downbeat" || r.method=="bar" || r.method=="state_wrapper")return remaining_models(r,input,result);
    if(r.method=="analysis" || r.method=="analysis_lifecycle" || r.method=="tracking" || r.method=="crf_wrapper")return remaining_tempo(r,input,result);
    return remaining_vectors(r,input,result);
}
}
