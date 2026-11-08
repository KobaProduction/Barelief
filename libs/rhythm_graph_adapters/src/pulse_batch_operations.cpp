// SPDX-License-Identifier: AGPL-3.0-or-later
#include "batch_operation_plan.h"
#include <cmath>
#include <climits>
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;using namespace rhythm_reference_ports;
namespace {
bool summary(std::span<NumericOperationOutput> out,const LagTempoConsensusResult& r) noexcept {
    return batch_scalar(out[0],r.bpm) && batch_scalar(out[1],r.uncorrected_bpm) && batch_scalar(out[2],r.octave_multiplier) && batch_vector(out[3],r.features);
}
class PulseTrain final:public BatchKernel<PulseTrain> {
public:
    bool prepare(const BatchPlan& p){plan_=p;return kernel_.prepare(p.rows,p.secondary);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        float lag=-1;if(!kernel_.process(in[0].contiguous_float32,in[1].contiguous_float32,lag))return false;
        return batch_scalar(out[0],lag) && batch_vector(out[1],kernel_.magnitudes()) && batch_vector(out[2],kernel_.variances()) && batch_vector(out[3],kernel_.scores());
    }
    PreparedPulseTrainScoring kernel_;
};
class Consensus final:public BatchKernel<Consensus> {
public:
    bool prepare(const BatchPlan& p){plan_=p;return kernel_.prepare(p.rate,p.classifier);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        kernel_.reset();
        for(std::size_t i=0;i<in[0].value.actual[0];++i){std::int32_t lag=0;if(signal_contracts::load_element(in[0].value,{0,i,0},lag)!=signal_contracts::Status::ok || !kernel_.add(lag))return false;}
        LagTempoConsensusResult value;if(!kernel_.finish(value))return false;
        return summary(out,value) && batch_vector(out[4],kernel_.histogram()) && batch_integer(out[5],kernel_.observations()) && batch_integer(out[6],value.lag);
    }
    PreparedLagTempoConsensus kernel_;
};
template<class T> std::unique_ptr<AdapterOperation> make(const BatchPlan& p){auto value=std::make_unique<T>();return value->prepare(p)?std::move(value):nullptr;}
}
std::unique_ptr<AdapterOperation> prepare_pulse_batch(const dataflow_runtime::NumericOperationNodeView&,const BatchPlan& p){
    switch(p.kind){case BatchKind::PulseTrain:return make<PulseTrain>(p);case BatchKind::LagConsensus:return make<Consensus>(p);default:return {};}
}
std::size_t pulse_batch_object_size(BatchKind kind) noexcept {
    switch(kind){case BatchKind::PulseTrain:return sizeof(PulseTrain);case BatchKind::LagConsensus:return sizeof(Consensus);default:return 0;}
}
}
