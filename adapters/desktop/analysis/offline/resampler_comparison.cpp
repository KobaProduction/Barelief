#include "resampler_comparison.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace desktop_analysis::offline {
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double,std::micro>(Clock::now()-start).count();
}
void synthetic_block(std::span<float> samples,std::size_t block,std::uint32_t rate) {
    for(std::size_t i=0;i<samples.size();++i){
        double value=0;
        for(unsigned tone=0;tone<8;++tone){
            const double frequency=130.+tone*tone*173.+std::sin(block*.03+tone)*11.;
            value+=.045*std::sin(6.283185307179586*frequency*(block*samples.size()+double(i))/rate);
        }
        samples[i]=float(value);
    }
}
bool measure_blocks(dsp_kernels::PreparedResampler& plan,const ComparisonRequest& request,const std::atomic_bool& cancel,std::vector<double>& durations){
    std::vector<float> input(request.block_samples),output(plan.maximum_output(request.block_samples,request.input_rate,plan.output_rate()));
    durations.reserve(request.blocks);
    for(std::size_t block=0;block<request.blocks&&!cancel.load();++block){
        synthetic_block(input,block,request.input_rate);std::size_t written=0;
        const auto started=Clock::now();
        if(!plan.process(input,input.size(),input.size(),output,output.size(),written))return false;
        durations.push_back(elapsed(started));
    }
    return true;
}
void duration_percentiles(std::vector<double>& durations,ComparisonResult& result){
    if(durations.empty())return;
    std::sort(durations.begin(),durations.end());
    result.p50=durations[durations.size()/2];result.p95=durations[durations.size()*95/100];
}
ComparisonResult measure(const ComparisonRequest& request,dsp_kernels::ResamplerMode mode,
                         const std::atomic_bool& cancel,const dsp_kernels::ResamplerBackendFactory* external) {
    dsp_kernels::PreparedResampler plan;
    const auto rate=mode==dsp_kernels::ResamplerMode::Bypass?request.input_rate:request.output_rate;
    const auto begin=Clock::now();
    const bool ready=plan.prepare({mode,request.input_rate,rate,1,request.quality},external);
    ComparisonResult result{mode,plan.output_rate(),elapsed(begin),0,0,plan.resources(),ready};
    if(!ready)return result;
    std::vector<double> durations;result.valid=measure_blocks(plan,request,cancel,durations);
    duration_percentiles(durations,result);
    return result;
}
}
std::vector<ComparisonResult> compare_resamplers(const ComparisonRequest& request,const std::atomic_bool& cancel,
                                                const dsp_kernels::ResamplerBackendFactory* external) {
    std::vector<ComparisonResult> result;
    if(!request.input_rate || !request.output_rate || request.quality>10 || !request.block_samples ||
       request.block_samples>65536 || !request.blocks || request.blocks>10000)return result;
    for(const auto& policy:signal_contracts::resampler_policies()){
        if(cancel.load())break;
        result.push_back(measure(request,policy.mode,cancel,external));
    }
    return result;
}
}
