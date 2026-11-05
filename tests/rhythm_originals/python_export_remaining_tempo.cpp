#include "python_export_remaining.h"
#include "python_export_benchmark.h"
#include "python_export_partitions.h"
#include <dsp_kernels/tempo_analysis_sequence.h>
#include <dsp_kernels/beat_tracking_sequence.h>
#include <dsp_kernels/crf_beat_sequence.h>

namespace originals {
namespace {
dsp_kernels::TempoAnalysisSequenceConfig analysis_config(const Request& r) {
    dsp_kernels::TempoAnalysisSequenceConfig c;c.histogram.method=dsp_kernels::TempoHistogramEstimator(r.size(0));
    c.histogram.frame_rate=50;c.histogram.minimum_bpm=100;c.histogram.maximum_bpm=150;c.histogram.history_seconds=2;
    c.histogram.online=r.size(1)!=0;c.activation_smoothing_seconds=.04f;c.histogram_smoothing=3;return c;
}
void analysis_outputs(const dsp_kernels::PreparedTempoAnalysisSequence& kernel,Result& result,std::string suffix) {
    std::vector<float> candidates;for(const auto& value:kernel.candidates()){candidates.push_back(value.tempo);candidates.push_back(value.strength);}
    result.floats("values"+suffix,candidates,{candidates.size()/2,2});
    result.floats("raw"+suffix,kernel.raw_histogram(),{kernel.raw_histogram().size()});
    result.floats("smoothed"+suffix,kernel.smoothed_histogram(),{kernel.smoothed_histogram().size()});
    result.floats("coordinates"+suffix,kernel.histogram_coordinates(),{kernel.histogram_coordinates().size()});
}
bool analysis(const Request& r,const Matrix& input,Result& result) {
    const auto c=analysis_config(r);dsp_kernels::PreparedTempoAnalysisSequence kernel;
    require(kernel.prepare(r.frames,c),"prepare actual tempo-analysis composition");bool ok=false;
    if(c.histogram.online){
        const auto first=r.size(2);require(first && first<input.values.size(),"explicit online partition");
        ok=kernel.process_online(std::span(input.values).first(first),true);if(!ok)return false;analysis_outputs(kernel,result,"0");
        ok=kernel.process_online(std::span(input.values).subspan(first),false);analysis_outputs(kernel,result,"1");
    } else {ok=kernel.process_offline(input.values);analysis_outputs(kernel,result,"");}
    report_workspace(result,kernel.workspace_bytes());return ok;
}
dsp_kernels::TempoAnalysisSequenceConfig lifecycle_config(const Request& r) {
    auto c=analysis_config(r);c.histogram.frame_rate=100;c.histogram.history_seconds=40.96f;
    c.histogram.online=true;c.activation_smoothing_seconds=.02f;return c;
}
bool analysis_steps(const Matrix& input,dsp_kernels::PreparedTempoAnalysisSequence& kernel,std::span<const ReplayPartition> partitions,Result& result) {
    for(std::size_t step=0;step<partitions.size();++step){
        const auto part=partitions[step];if(!kernel.process_online(std::span(input.values).subspan(part.first,part.count),part.reset))return false;
        analysis_outputs(kernel,result,std::to_string(step));
    }
    return true;
}

bool analysis_lifecycle(const Request& r,const Matrix& input,Result& result) {
    const auto c=lifecycle_config(r);dsp_kernels::PreparedTempoAnalysisSequence kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,c,{},{},{},b);},[&]{return kernel.prepare(r.frames,c);}),"prepare long-history analysis");
    const auto chunks=sidecar(r,".chunks",1,r.size(1)),resets=sidecar(r,".resets",1,r.size(1));
    const auto partitions=replay_partitions(chunks.values,resets.values,input.values.size());
    if(benchmark::enabled() && !benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return replay(input.values,partitions,[&](auto values,bool reset){return kernel.process_online(values,reset);});}))return false;
    if(!analysis_steps(input,kernel,partitions,result))return false;
    report_workspace(result,kernel.workspace_bytes());return true;
}
bool tracking(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::BeatTrackingSequenceConfig c;c.histogram.frame_rate=50;c.histogram.minimum_bpm=100;c.histogram.maximum_bpm=150;
    c.histogram.history_seconds=2;c.activation_smoothing_seconds=.04f;c.histogram_smoothing=3;c.look_ahead_seconds=.5f;c.variable_tempo=r.size(0)!=0;
    dsp_kernels::PreparedBeatTrackingSequence kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,c,b);},[&]{return kernel.prepare(r.frames,c);}),"prepare interval tracking composition");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values);});result.floats("times",kernel.events(),{kernel.events().size()});
    result.integers("positions",kernel.positions());report_workspace(result,kernel.workspace_bytes());return ok;
}
dsp_kernels::CrfBeatSequenceConfig crf_config(const Request& r) {
    dsp_kernels::CrfBeatSequenceConfig c;auto& h=c.tempo.histogram;
    h.frame_rate=50;h.minimum_bpm=60;h.maximum_bpm=240;h.history_seconds=2;
    c.tempo.activation_smoothing_seconds=.04f;c.tempo.histogram_smoothing=3;
    c.use_factors=r.size(0)!=0;c.num_intervals=r.size(1);c.interval_sigma=float(r.number(2));return c;
}
bool crf(const Request& r,const Matrix& input,Result& result) {
    const auto c=crf_config(r);dsp_kernels::PreparedCrfBeatSequence kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,c,dsp_kernels::default_beat_interval_factors,b);},[&]{return kernel.prepare(r.frames,c);}),"prepare complete CRF wrapper");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values);});std::vector<float> candidates;
    for(const auto& item:kernel.tempo_candidates()){candidates.push_back(item.tempo);candidates.push_back(item.strength);}
    result.floats("times",kernel.events(),{kernel.events().size()});result.integers("positions",kernel.positions());
    result.integers("intervals",kernel.trial_intervals());result.floats("scores",kernel.normalized_scores(),{kernel.normalized_scores().size()});
    result.floats("tempo_candidates",candidates,{candidates.size()/2,2});report_workspace(result,kernel.workspace_bytes());return ok;
}
}
bool remaining_tempo(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="analysis_lifecycle")return analysis_lifecycle(r,input,result);
    if(r.method=="crf_wrapper")return crf(r,input,result);
    return r.method=="analysis"?analysis(r,input,result):tracking(r,input,result);
}
}
