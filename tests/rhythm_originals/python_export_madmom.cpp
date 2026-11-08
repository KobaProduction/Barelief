#include "python_export.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/spectral_novelty.h>
#include <dsp_kernels/activation_peaks.h>
#include <dsp_kernels/interval_histogram.h>
#include <algorithm>

namespace originals {
namespace {
bool novelty(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::SpectralNoveltyConfig c;c.method=dsp_kernels::SpectralNoveltyMethod(r.size(0));c.lag=r.size(1);
    c.frequency_maximum=r.size(2);c.temporal_maximum=r.size(3);c.temporal_origin=std::ptrdiff_t(r.number(4));
    Request phase_request=r;phase_request.input=r.input.string()+".phase";const auto phase=read_input(phase_request);
    dsp_kernels::PreparedSpectralNovelty kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(std::max(r.frames,std::size_t{1}),r.columns,r.columns,c,0,b);},
        [&]{return kernel.prepare(std::max(r.frames,std::size_t{1}),r.columns,r.columns,c);}),"prepare spectral novelty");
    Matrix output(r.frames,kernel.output_features());const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.source(),phase.source(),r.frames,output.destination());});
    result.floats("values",output.values,{r.frames,output.columns});return ok;
}
bool histogram(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::IntervalHistogramConfig c;c.method=dsp_kernels::IntervalHistogramMethod(r.size(0));c.minimum_interval=r.size(1);
    c.maximum_interval=r.size(2);c.alpha=float(r.number(3));c.channels=r.columns;c.history_frames=r.frames;
    dsp_kernels::PreparedIntervalHistogram kernel;require(benchmark::prepare([&](std::size_t& bytes){return decltype(kernel)::storage_bytes(r.frames,c,{},bytes);},[&]{return kernel.prepare(r.frames,c);}),"prepare interval histogram primitive");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process_offline(input.values);});result.floats("values",kernel.bins(),{r.columns,kernel.intervals().size()});
    result.integers("intervals",kernel.intervals());return ok;
}
}
bool madmom(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="novelty")return novelty(r,input,result);
    if(r.method=="histogram")return histogram(r,input,result);
    require(false,"unknown madmom method");return false;
}
}
