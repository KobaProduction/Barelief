#include "python_export_next.h"
#include "python_export_benchmark.h"
#include <dsp_kernels/pcm_pattern_recipe.h>
#include <algorithm>

namespace originals {
namespace {
struct PatternInputs {
    Matrix weights,means,covariances,beats,tempo;
    explicit PatternInputs(const Request& r):
        weights(sidecar(r,".weights",r.size(0)*r.size(1),r.size(2))),
        means(sidecar(r,".means",r.size(0)*r.size(1)*r.size(2),2)),
        covariances(sidecar(r,".covariances",r.size(0)*r.size(1)*r.size(2),2)),
        beats(sidecar(r,".beats",1,r.size(0))),tempo(sidecar(r,".tempo",r.size(0),4)){}
};
std::vector<float> section(const std::vector<float>& input,std::size_t first,std::size_t count) {
    require(first<=input.size() && count<=input.size()-first,"model sidecar range");
    return {input.begin()+std::ptrdiff_t(first),input.begin()+std::ptrdiff_t(first+count)};
}
dsp_kernels::GaussianMixtureData mixture(const PatternInputs& input,std::size_t index,std::size_t components) {
    dsp_kernels::GaussianMixtureData result;result.features=2;result.components=components;
    result.covariance=dsp_kernels::GaussianCovariance::Diagonal;
    result.weights=section(input.weights.values,index*components,components);
    result.means=section(input.means.values,index*components*2,components*2);
    result.covariances=section(input.covariances.values,index*components*2,components*2);return result;
}
std::vector<dsp_kernels::GmmPatternData> model_data(const Request& r,const PatternInputs& input) {
    std::vector<dsp_kernels::GmmPatternData> result(r.size(0));
    for(std::size_t p=0;p<result.size();++p){
        result[p].beats=std::uint32_t(input.beats.values[p]);
        for(std::size_t g=0;g<r.size(1);++g)result[p].gmms.push_back(mixture(input,p*r.size(1)+g,r.size(2)));
    }
    return result;
}
std::vector<dsp_kernels::PatternTempoParameters> model_tempi(const PatternInputs& input) {
    std::vector<dsp_kernels::PatternTempoParameters> result(input.tempo.values.size()/4);
    for(std::size_t p=0;p<result.size();++p){
        result[p].minimum_bpm=input.tempo.values[4*p];result[p].maximum_bpm=input.tempo.values[4*p+1];
        result[p].num_tempi=std::size_t(input.tempo.values[4*p+2]);result[p].transition.lambda=input.tempo.values[4*p+3];
    }
    return result;
}
void model_outputs(const dsp_kernels::PreparedPatternStateDecoder& kernel,Result& result) {
    result.floats("times",kernel.beat_times(),{kernel.beat_times().size()});result.states("numbers",kernel.beat_numbers());
    result.states("path",kernel.state_path());const float score=kernel.log_probability();result.floats("log_probability",std::span(&score,1),{1});
}
Matrix feature_rows(const Request& r,const Matrix& input,Result& result,bool measured=true) {
    dsp_kernels::PreparedPatternFeatureRecipe kernel;
    const bool prepared=measured?benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(input.values.size(),b);},[&]{return kernel.prepare(input.values.size());}):kernel.prepare(input.values.size());
    require(prepared,"prepare original pattern frontend");Matrix features(kernel.frames(input.values.size()),kernel.features());
    require(measured?benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values,features.destination());}):kernel.process(input.values,features.destination()),"pattern frontend");
    result.floats("features",features.values,{features.values.size()/features.columns,features.columns});return features;
}
bool supplied_features(const Request& r,const Matrix& input,Result& result) {
    const PatternInputs fixtures(r);const auto patterns=model_data(r,fixtures);const auto tempi=model_tempi(fixtures);
    dsp_kernels::PreparedPatternStateDecoder kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(r.frames,patterns,tempi,50,b);},[&]{return kernel.prepare(r.frames,patterns,tempi,50);}),"prepare supplied pattern model");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.source(),r.frames);});model_outputs(kernel,result);return ok;
}
bool pcm_pattern(const Request& r,const Matrix& input,Result& result) {
    const PatternInputs fixtures(r);const auto patterns=model_data(r,fixtures);const auto tempi=model_tempi(fixtures);
    dsp_kernels::PreparedPcmPatternRecipe kernel;
    require(benchmark::prepare([&](std::size_t& b){return decltype(kernel)::storage_bytes(input.values.size(),patterns,tempi,b);},[&]{return kernel.prepare(input.values.size(),patterns,tempi);}),"prepare full PCM pattern model");
    const bool ok=benchmark::measured_process(r.prefix,kernel.workspace_bytes(),[&]{return kernel.process(input.values);});model_outputs(kernel.result(),result);feature_rows(r,input,result,false);return ok;
}
}
bool patterns(const Request& r,const Matrix& input,Result& result) {
    if(r.method=="features"){feature_rows(r,input,result);return true;}
    if(r.method=="state")return supplied_features(r,input,result);
    if(r.method=="pcm")return pcm_pattern(r,input,result);
    require(false,"unknown pattern recipe");return false;
}
}
