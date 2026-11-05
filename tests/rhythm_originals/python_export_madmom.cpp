#include "python_export.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/spectral_novelty.h>
#include <dsp_kernels/activation_peaks.h>
#include <dsp_kernels/activation_peak_matrix.h>
#include <dsp_kernels/interval_histogram.h>
#include <dsp_kernels/tempo_histogram_sequence.h>
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
dsp_kernels::ActivationPeakConfig peak_config(const Request& r) {
    dsp_kernels::ActivationPeakConfig c;c.threshold=float(r.number(0));c.pre_average=r.size(1);c.post_average=r.size(2);
    c.pre_maximum=r.size(3);c.post_maximum=r.size(4);return c;
}
bool peaks(const Request& r,const Matrix& input,Result& result) {
    const auto c=peak_config(r);dsp_kernels::PreparedActivationPeakMatrix kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(std::max(r.frames,std::size_t{1}),r.columns,c,{},b);},
        [&]{return kernel.prepare(std::max(r.frames,std::size_t{1}),r.columns,c);}),"prepare activation peaks");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.source(),r.frames,r.columns);});std::vector<std::size_t> frames,channels;
    for(const auto& value:kernel.positions()){frames.push_back(value.frame);channels.push_back(value.channel);}
    result.integers("frames",frames);result.integers("channels",channels);return ok;
}
dsp_kernels::TempoHistogramSequenceConfig histogram_sequence_config(const Request& r) {
    dsp_kernels::TempoHistogramSequenceConfig c;c.method=dsp_kernels::TempoHistogramEstimator::Comb;
    c.frame_rate=100;c.minimum_bpm=100;c.maximum_bpm=500;c.history_seconds=1.92f;c.alpha=float(r.number(3));return c;
}

bool histogram_sequence(const Request& r,const Matrix& input,Result& result) {
    require(r.size(0)==1 && r.size(1)==12 && r.size(2)==60 && r.frames==192 && r.columns==1,"explicit canonical whole histogram configuration");
    const auto c=histogram_sequence_config(r);dsp_kernels::PreparedTempoHistogramSequence kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,c,{},b);},[&]{return kernel.prepare(r.frames,c);}),"prepare actual whole tempo histogram");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process_offline(input.values);});
    result.floats("values",kernel.bins(),{1,kernel.bins().size()});std::vector<std::size_t> intervals;
    for(float value:kernel.coordinates())intervals.push_back(std::size_t(value));result.integers("intervals",intervals);return ok;
}
bool histogram(const Request& r,const Matrix& input,Result& result) {
    if(benchmark::enabled())return histogram_sequence(r,input,result);
    dsp_kernels::IntervalHistogramConfig c;c.method=dsp_kernels::IntervalHistogramMethod(r.size(0));c.minimum_interval=r.size(1);
    c.maximum_interval=r.size(2);c.alpha=float(r.number(3));c.channels=r.columns;c.history_frames=r.frames;
    dsp_kernels::PreparedIntervalHistogram kernel;require(kernel.prepare(r.frames,c),"prepare interval histogram");
    const bool ok=kernel.process_offline(input.values);result.floats("values",kernel.bins(),{r.columns,kernel.intervals().size()});
    result.integers("intervals",kernel.intervals());return ok;
}
}
bool madmom(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="novelty")return novelty(r,input,result);
    if(r.method=="peaks")return peaks(r,input,result);
    if(r.method=="histogram")return histogram(r,input,result);
    require(false,"unknown madmom method");return false;
}
}
