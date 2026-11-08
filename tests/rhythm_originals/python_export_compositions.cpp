#include "python_export_next.h"
#include "python_export_options.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/tempo_estimator.h>
#include <optional>

namespace originals {
Matrix sidecar(const Request& r,std::string_view suffix,std::size_t rows,std::size_t columns) {
    auto request=r;request.input=r.input.string()+std::string(suffix);request.frames=rows;request.columns=columns;
    return read_input(request);
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
}
bool compositions(const Request& request,const Matrix& input,Result& result) {
    return request.method=="tempo_options" && tempo_options(request,input,result);
}
}
