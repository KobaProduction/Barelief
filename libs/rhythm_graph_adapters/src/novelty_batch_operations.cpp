// SPDX-License-Identifier: AGPL-3.0-or-later
#include "batch_operation_plan.h"
#include <algorithm>
namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;using namespace rhythm_reference_ports;using namespace signal_contracts;
namespace {
bool write_band_stages(const PreparedBandNovelty& kernel,std::size_t frames,std::size_t bands,std::span<NumericOperationOutput> out) noexcept {
    out[1].value.actual={frames-1,bands};
    for(std::size_t band=0;band<bands;++band){const auto values=kernel.band_novelty(band);for(std::size_t row=0;row<values.size();++row)if(store_element(out[1].value,{0,row,band},values[row])!=Status::ok)return false;}
    const auto weights=kernel.weights();out[2].value.actual={weights.size()/bands,bands};
    for(std::size_t i=0;i<weights.size();++i)if(store_element(out[2].value,{0,i/bands,i%bands},weights[i])!=Status::ok)return false;
    return true;
}
class BandNovelty final:public BatchKernel<BandNovelty> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n,const BatchPlan& p){plan_=p;if(!kernel_.prepare(p.rows,p.columns,p.band,array_parameter(n,"weights")))return false;output_.resize(p.rows-1);return true;}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+output_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto frames=in[0].value.actual[0];if(!frames || in[0].value.actual[1]!=plan_.columns)return false;
        auto result=std::span(output_).first(frames-1);
        if(!kernel_.process(batch_rows(in[0]),frames,result) || !batch_vector(out[0],result))return false;
        return write_band_stages(kernel_,frames,plan_.columns,out);
    }
    PreparedBandNovelty kernel_;std::vector<float> output_;
};
class Fixed final:public BatchKernel<Fixed> {
public:
    bool prepare(const BatchPlan& p){plan_=p;return kernel_.prepare(p.rows,p.fixed);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(!kernel_.process(in[0].contiguous_float32))return false;
        return batch_peaks(out[0],kernel_.candidates()) && batch_vector(out[1],kernel_.correlation()) && batch_vector(out[2],kernel_.smoothed_correlation()) &&
            batch_status(out[3],std::uint8_t(kernel_.status())) && batch_scalar(out[4],kernel_.threshold()) && batch_integer(out[5],kernel_.refinements()) &&
            batch_peaks(out[6],kernel_.initial_peaks()) && batch_vector(out[7],kernel_.distribution_histogram()) && batch_vector(out[8],kernel_.distribution_centers());
    }
    PreparedFixedTempoCandidates kernel_;
};
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
std::unique_ptr<AdapterOperation> prepare_novelty_batch(const domain::ResolvedProcessingNode& n,const BatchPlan& p){
    if(p.kind==BatchKind::BandNovelty){auto value=std::make_unique<BandNovelty>();return value->prepare(n,p)?std::move(value):nullptr;}
    if(p.kind==BatchKind::FixedCandidates){auto value=std::make_unique<Fixed>();return value->prepare(p)?std::move(value):nullptr;}
    if(p.kind==BatchKind::FourierCandidates){auto value=std::make_unique<FourierCandidates>();return value->prepare(p)?std::move(value):nullptr;}
    if(p.kind==BatchKind::WeightedHistogram){auto value=std::make_unique<Weighted>();return value->prepare(p)?std::move(value):nullptr;}
    return {};
}
std::size_t novelty_batch_object_size(BatchKind kind) noexcept {
    switch(kind){case BatchKind::BandNovelty:return sizeof(BandNovelty);case BatchKind::FixedCandidates:return sizeof(Fixed);case BatchKind::FourierCandidates:return sizeof(FourierCandidates);case BatchKind::WeightedHistogram:return sizeof(Weighted);default:return 0;}
}
}
