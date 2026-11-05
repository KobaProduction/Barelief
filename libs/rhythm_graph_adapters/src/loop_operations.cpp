// SPDX-License-Identifier: AGPL-3.0-or-later
#include "loudness_operations.h"
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace dataflow_runtime;using namespace rhythm_reference_ports;
namespace {
bool durations(NumericOperationOutput& out,const std::array<std::int64_t,4>& values) noexcept {
    if(!loudness_shape(out,4))return false;
    for(std::size_t k=0;k<4;++k)if(store_element(out.value,{0,k,0},values[k])!=Status::ok)return false;return true;
}
bool fit_outputs(std::span<NumericOperationOutput> out,const LoopLengthConfidenceResult& value) noexcept {
    return batch_scalar(out[0],value.confidence) && batch_scalar(out[1],value.rounded_bpm) && batch_integer(out[2],value.start_sample) &&
        batch_integer(out[3],value.end_sample) && durations(out[4],value.durations) && batch_vector(out[5],value.scores);
}
class LoopFit final:public LoudnessKernel<LoopFit> {
public:
    bool prepare(const LoudnessPlan& p){plan_=p;return kernel_.prepare(p.rate);}
    std::size_t workspace_bytes() const noexcept override {return 0;}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        float bpm=0;LoopLengthConfidenceResult result;
        return load_element(in[1].value,{0,0,0},bpm)==Status::ok && kernel_.process(in[0].contiguous_float32,bpm,result) && fit_outputs(out,result);
    }
    PreparedLoopLengthConfidence kernel_;
};
class LoopTempo final:public LoudnessKernel<LoopTempo> {
public:
    bool prepare(const LoudnessPlan& p){plan_=p;return kernel_.prepare(p.rows,p.threshold);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(!kernel_.process(in[0].contiguous_float32))return false;const auto& fit=kernel_.confidence();
        return batch_scalar(out[0],kernel_.bpm()) && batch_scalar(out[1],kernel_.raw_bpm()) && batch_scalar(out[2],fit.confidence) && batch_scalar(out[3],fit.rounded_bpm) &&
            batch_integer(out[4],fit.start_sample) && batch_integer(out[5],fit.end_sample) && durations(out[6],fit.durations) && batch_vector(out[7],fit.scores) && batch_vector(out[8],kernel_.tempo().lag_histogram());
    }
    PreparedLoopTempoRecipe kernel_;
};
template<class T> std::unique_ptr<AdapterOperation> make(const LoudnessPlan& p){auto value=std::make_unique<T>();return value->prepare(p)?std::move(value):nullptr;}
}
std::unique_ptr<AdapterOperation> prepare_loop_kernel(const domain::ResolvedProcessingNode&,const LoudnessPlan& p){
    return p.kind==LoudnessKind::LoopFit?make<LoopFit>(p):make<LoopTempo>(p);
}
std::size_t loop_kernel_size(LoudnessKind kind) noexcept {return kind==LoudnessKind::LoopFit?sizeof(LoopFit):sizeof(LoopTempo);}
}
