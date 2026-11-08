#include "python_export.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/onset_strength.h>
#include <dsp_kernels/tempo_estimator.h>
#include <dsp_kernels/beat_tracker.h>
#include <algorithm>
namespace originals {
namespace {
bool strength(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::OnsetStrengthConfig c;c.lag=r.size(0);c.max_size=r.size(1);c.center=r.size(2)!=0;c.detrend=r.size(3)!=0;
    c.aggregate=dsp_kernels::OnsetAggregate(r.size(4));c.n_fft=r.size(5);c.hop_length=r.size(6);
    dsp_kernels::PreparedOnsetStrength kernel;const auto frames=std::max(r.frames,std::size_t{1});
    require(benchmark::prepare([&](std::size_t& bytes){return decltype(kernel)::storage_bytes(r.columns,frames,c,bytes);},
        [&]{return kernel.prepare(r.columns,frames,c);}),"prepare onset strength");
    std::vector<float> output(kernel.output_frames(r.frames));const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.source(),r.frames,output);});
    result.floats("values",output,{output.size()});report_workspace(result,kernel.workspace_bytes());return ok;
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
}
bool librosa(const Request& request,const Matrix& input,Result& result) {
    if(request.method=="strength")return strength(request,input,result);
    if(request.method=="tempo")return tempo(request,input,result);
    if(request.method=="beat")return beat(request,input,result);
    require(false,"unknown librosa method");return false;
}
}
