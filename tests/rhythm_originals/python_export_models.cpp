#include "python_export.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/gaussian_mixture.h>
#include <dsp_kernels/sparse_markov.h>
#include <dsp_kernels/interval_sequence_decoder.h>
#include <dsp_kernels/beat_state_decoder.h>
#include <algorithm>
#include <numeric>

namespace originals {
namespace {
Matrix sidecar(const Request& r,std::string_view suffix,std::size_t rows,std::size_t columns) {
    auto copy=r;copy.input=r.input.string()+std::string(suffix);copy.frames=rows;copy.columns=columns;return read_input(copy);
}
bool score_rows(dsp_kernels::PreparedGaussianMixture& kernel,const Matrix& input,Matrix& posterior,Matrix& components,std::span<float> scores) {
    for(std::size_t row=0;row<scores.size();++row){
        if(!kernel.process(std::span(input.values).subspan(row*input.columns,input.columns),scores[row],std::span(posterior.values).subspan(row*posterior.columns,posterior.columns)))return false;
        std::copy(kernel.component_log_probabilities().begin(),kernel.component_log_probabilities().end(),components.values.begin()+std::ptrdiff_t(row*components.columns));
    }
    return true;
}
bool gmm(const Request& r,const Matrix& input,Result& result) {
    const auto components=r.size(1);const auto covariance=dsp_kernels::GaussianCovariance(r.size(0));
    const auto weights=sidecar(r,".weights",components,1),means=sidecar(r,".means",components,r.columns);
    const auto width=covariance==dsp_kernels::GaussianCovariance::Diagonal?r.columns:covariance==dsp_kernels::GaussianCovariance::Spherical?1:r.columns*r.columns;
    const auto covariances=sidecar(r,".covariances",covariance==dsp_kernels::GaussianCovariance::Tied?1:components,width);
    const dsp_kernels::GaussianMixtureParameters parameters{r.columns,components,covariance,weights.values,means.values,covariances.values};const auto retry=float(r.number(2,1e-7));
    dsp_kernels::PreparedGaussianMixture kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(parameters,retry,b);},[&]{return kernel.prepare(parameters,retry);}),"prepare GMM");
    Matrix posterior(r.frames,components),component_values(r.frames,components);std::vector<float> scores(r.frames);
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return score_rows(kernel,input,posterior,component_values,scores);});
    result.floats("scores",scores,{r.frames});result.floats("responsibilities",posterior.values,{r.frames,components});
    result.floats("components",component_values.values,{r.frames,components});report_workspace(result,kernel.workspace_bytes());return ok;
}
std::vector<dsp_kernels::MarkovTransition> edges(const Matrix& dense) {
    std::vector<dsp_kernels::MarkovTransition> result;
    for(std::size_t from=0;from<dense.columns;++from)for(std::size_t to=0;to<dense.columns;++to){
        const float value=dense.values[from*dense.columns+to];if(value>0)result.push_back({std::uint32_t(from),std::uint32_t(to),value});
    }
    return result;
}
bool hmm(const Request& r,const Matrix& input,Result& result) {
    const auto transitions=sidecar(r,".transitions",r.columns,r.columns),initial=sidecar(r,".initial",1,r.columns);
    std::vector<std::uint32_t> pointers(r.columns);std::iota(pointers.begin(),pointers.end(),0);
    dsp_kernels::PreparedSparseMarkovModel kernel;const auto links=edges(transitions);
    require(kernel.prepare(r.frames,r.columns,pointers,links,initial.values),"prepare HMM");
    if(r.method=="hmm_forward"){
        Matrix posterior(r.frames,r.columns);const bool ok=kernel.forward(input.source(),r.frames,true,posterior.destination());
        result.floats("posteriors",posterior.values,{r.frames,r.columns});return ok;
    }
    const bool ok=kernel.viterbi(input.source(),r.frames);result.states("path",kernel.state_path());
    const float probability=kernel.log_probability();result.floats("log_probability",std::span(&probability,1),{1});return ok;
}
bool crf(const Request& r,const Matrix& input,Result& result) {
    const auto interval=r.size(0);dsp_kernels::PreparedIntervalSequenceDecoder kernel;
    require(kernel.prepare(r.frames,interval,interval,float(r.number(1))),"prepare CRF interval");
    const bool ok=kernel.process(input.values,interval);result.integers("positions",kernel.positions());
    const float probability=kernel.log_probability();result.floats("log_probability",std::span(&probability,1),{1});
    result.floats("transition",kernel.transition_distribution(),{kernel.transition_distribution().size()});return ok;
}
bool beat_states(const Request& r,const Matrix& input,Result& result) {
    dsp_kernels::BeatStateDecoderConfig c;c.frame_rate=float(r.number(0));c.min_bpm=float(r.number(1));c.max_bpm=float(r.number(2));
    c.num_intervals=r.size(3);c.correct=r.size(4)!=0;dsp_kernels::PreparedBeatStateDecoder kernel;
    require(kernel.prepare(r.frames,c),"prepare beat state decoder");std::vector<std::uint8_t> mask(r.frames);
    const bool ok=kernel.offline(input.values,mask);result.mask("mask",mask);result.states("path",kernel.state_path());
    result.states("intervals",kernel.intervals());return ok;
}
}
bool models(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="gmm")return gmm(r,input,result);
    if(r.method=="hmm_viterbi" || r.method=="hmm_forward")return hmm(r,input,result);
    if(r.method=="crf")return crf(r,input,result);
    if(r.method=="beat_states")return beat_states(r,input,result);
    require(false,"unknown model method");return false;
}
}
