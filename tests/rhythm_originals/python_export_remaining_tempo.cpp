#include "python_export_remaining.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/interval_beat_search.h>
#include <dsp_kernels/interval_sequence_search.h>

namespace originals {
namespace {
bool tracking(const Request& r,const Matrix& input,Result& result) {
    const auto interval=r.size(0);dsp_kernels::PreparedIntervalBeatSearch kernel;
    require(benchmark::prepare([&](std::size_t& bytes){return decltype(kernel)::storage_bytes(r.frames,interval,.2f,bytes);},[&]{return kernel.prepare(r.frames,interval,.2f);}),"prepare explicit global interval search");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,interval);});
    result.integers("positions",kernel.positions());report_workspace(result,kernel.workspace_bytes());return ok;
}
bool crf(const Request& r,const Matrix& input,Result& result) {
    const auto candidates=sidecar(r,".intervals",1,r.size(0));std::vector<std::size_t> intervals;
    for(float value:candidates.values){require(value>=1 && value<float(SIZE_MAX),"explicit positive trial interval");intervals.push_back(std::size_t(value));}
    dsp_kernels::PreparedIntervalSequenceSearch kernel;
    require(benchmark::prepare([&](std::size_t& bytes){return decltype(kernel)::storage_bytes(r.frames,12,60,intervals.size(),.18f,bytes);},[&]{return kernel.prepare(r.frames,12,60,intervals.size(),.18f);}),"prepare explicit CRF trials");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,intervals);});
    result.integers("positions",kernel.positions());result.integers("intervals",kernel.trial_intervals());
    result.floats("scores",kernel.normalized_scores(),{kernel.normalized_scores().size()});report_workspace(result,kernel.workspace_bytes());return ok;
}
}
bool remaining_tempo(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="interval_search")return tracking(r,input,result);
    if(r.method=="crf_search")return crf(r,input,result);
    require(false,"unknown explicit interval decoder");return false;
}
}
