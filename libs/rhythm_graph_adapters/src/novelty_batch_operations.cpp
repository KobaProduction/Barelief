// SPDX-License-Identifier: AGPL-3.0-or-later
#include "batch_operation_plan.h"
#include <algorithm>
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;using namespace rhythm_reference_ports;using namespace signal_contracts;
namespace {
class FourierCandidates final:public BatchKernel<FourierCandidates> {
public:
    bool prepare(const BatchPlan& p){plan_=p;const auto& c=p.histogram;return kernel_.prepare(p.rows,{c.path.frequency_step,c.path.minimum_bpm,c.path.maximum_bpm,c.maximum_peaks});}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        return kernel_.process(in[0].contiguous_float32) && batch_peaks(out[0],kernel_.spectral_peaks()) && batch_peaks(out[1],kernel_.bpm_observations()) && batch_vector(out[2],kernel_.tempo_row());
    }
    PreparedFourierTempoCandidates kernel_;
};
class Weighted final:public BatchKernel<Weighted> {
public:
    bool prepare(const BatchPlan& p){plan_=p;const auto& c=p.histogram;return kernel_.prepare(p.rows,p.secondary,{c.path.minimum_bpm,c.path.maximum_bpm,c.path.tolerance_percent,c.weight_by_magnitude});}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(in[0].value.actual[1]!=2 || in[1].value.actual[1]!=plan_.shape[0].rows)return false;
        return kernel_.process(batch_rows(in[0]),in[0].value.actual[0],batch_rows(in[1]),in[1].value.actual[0]) &&
            batch_vector(out[0],kernel_.histogram()) && batch_peaks(out[1],kernel_.candidates()) && batch_status(out[2],std::uint8_t(kernel_.silent()));
    }
    PreparedWeightedTempoHistogram kernel_;
};
}
std::unique_ptr<AdapterOperation> prepare_novelty_batch(const dataflow_runtime::NumericOperationNodeView&,const BatchPlan& p){
    if(p.kind==BatchKind::FourierCandidates){auto value=std::make_unique<FourierCandidates>();return value->prepare(p)?std::move(value):nullptr;}
    if(p.kind==BatchKind::WeightedHistogram){auto value=std::make_unique<Weighted>();return value->prepare(p)?std::move(value):nullptr;}
    return {};
}
std::size_t novelty_batch_object_size(BatchKind kind) noexcept {
    switch(kind){case BatchKind::FourierCandidates:return sizeof(FourierCandidates);case BatchKind::WeightedHistogram:return sizeof(Weighted);default:return 0;}
}
}
