#include "operation_support.h"
#include "sequence_settings.h"
#include <dsp_kernels/envelope_tempo.h>
#include <dsp_kernels/tempo_ratio_sequence.h>
#include <dsp_kernels/metrical_tempogram_sequence.h>
#include <algorithm>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
bool tempo_config(const domain::ResolvedProcessingNode& n,TempoEstimatorConfig& c) noexcept {
    std::size_t mode=1;
    if(!feature_rate(n,44100.f/512,c.frame_rate) || !number(n,"start_bpm",120,c.start_bpm) ||
       !number(n,"std_bpm",1,c.std_bpm) || !number(n,"max_tempo",320,c.max_tempo) ||
       !flag(n,"limit_max_tempo",true,c.limit_max_tempo) || !integer(n,"aggregation",1,mode) || mode>2)return false;
    c.aggregate=TempoAggregation(mode);return true;
}
bool envelope_config(const domain::ResolvedProcessingNode& n,EnvelopeTempoConfig& c) noexcept {
    float seconds=8;
    if(!tempo_config(n,c.estimator) || !number(n,"sample_rate",44100,c.sample_rate) ||
       !integer(n,"hop_samples",512,c.hop_samples) || !number(n,"autocorrelation_seconds",8,seconds))return false;
    c.autocorrelation_seconds=seconds;return true;
}
bool lag_geometry(const domain::ResolvedProcessingNode& n,std::size_t& rows,std::size_t& columns) noexcept {
    if(input_geometry(n,0,Rank::Matrix,rows,columns))return true;
    std::size_t unused=0;
    if(!input_geometry(n,0,Rank::Vector,columns,unused))return false;
    rows=1;return true;
}
bool read_lags(const void* context,std::size_t row,std::span<float> output) noexcept {
    const auto& source=*static_cast<const NumericOperationInput*>(context);
    if(source.value.contract->shape.rank==Rank::Matrix)return read_row(context,row,output);
    if(row || output.size()!=source.value.actual[0])return false;
    for(std::size_t i=0;i<output.size();++i)if(load_element(source.value,{0,i,0},output[i])!=Status::ok)return false;
    return true;
}
bool has_activity(const NumericOperationInput& input) noexcept {
    for(std::size_t row=0;row<input.value.actual[0];++row)for(std::size_t column=0;column<input.value.actual[1];++column){
        float value=0;if(load_element(input.value,{0,row,column},value)!=Status::ok || value!=0)return true;
    }
    return false;
}
class Tempo final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Tempo>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;TempoEstimatorConfig c;
        if(!lag_geometry(n,a,b) || !tempo_config(n,c) || !flag(n,"silence_zero",false,silence_zero_) || !kernel_.prepare(a,b,c))return false;
        aggregated_=c.aggregate!=TempoAggregation::None;output_.resize(aggregated_?1:a);return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+output_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& value=*input_at(in,0);const auto* prior=available_input_at(in,1);
        const auto frames=value.value.contract->shape.rank==Rank::Vector?1:value.value.actual[0];
        const auto count=aggregated_?1:frames;
        auto result=std::span(output_).first(count);
        if(silence_zero_ && !has_activity(value)){std::fill(result.begin(),result.end(),0.f);return write_values(*output_at(out,0),result);}
        return kernel_.process({&value,read_lags},frames,result,prior?prior->contiguous_float32:std::span<const float>{}) && write_values(*output_at(out,0),result);
    }
    PreparedTempoEstimator kernel_;std::vector<float> output_;bool aggregated_=true,silence_zero_=false;
};
class EnvelopeTempo final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<EnvelopeTempo>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;EnvelopeTempoConfig c;
        if(!input_geometry(n,0,Rank::Vector,a,b) || !envelope_config(n,c) || !kernel_.prepare(a,c))return false;
        output_.resize(kernel_.output_frames(a));return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+output_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& value=*input_at(in,0);const auto* prior=available_input_at(in,1);auto result=std::span(output_).first(kernel_.output_frames(value.value.actual[0]));
        return kernel_.process(value.contiguous_float32,result,prior?prior->contiguous_float32:std::span<const float>{}) && write_values(*output_at(out,0),result);
    }
    PreparedEnvelopeTempo kernel_;std::vector<float> output_;
};
bool prior_contract(const domain::ResolvedProcessingNode& n,std::size_t lags) noexcept {
    if(!contract(n,domain::PortDirection::Input,1))return true;
    std::size_t a=0,b=0;return input_geometry(n,1,Rank::Vector,a,b) && a==lags;
}
}
PreparedSequence prepare_tempo(const domain::ConfiguredProcessingNode& node) {
    const auto n=definition(node);
    if(n.kind=="TempoEstimate"){auto op=std::make_unique<Tempo>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="EnvelopeTempo"){auto op=std::make_unique<EnvelopeTempo>();return op->prepare(n)?std::move(op):nullptr;}
    return prepare_tempo_ratio(node);
}
namespace {
struct TempoStorage {std::size_t bytes=0,count=0,object=0;};
bool estimator_storage(const domain::ResolvedProcessingNode& node,TempoStorage& storage) noexcept {
    std::size_t rows=0,lags=0;TempoEstimatorConfig config;
    if(!lag_geometry(node,rows,lags) || !tempo_config(node,config) || !prior_contract(node,lags) ||
       !PreparedTempoEstimator::storage_bytes(rows,lags,config,storage.bytes))return false;
    storage.count=config.aggregate==TempoAggregation::None?rows:1;storage.object=sizeof(Tempo);return true;
}
bool envelope_storage(const domain::ResolvedProcessingNode& node,TempoStorage& storage) noexcept {
    std::size_t rows=0,columns=0,lags=0;EnvelopeTempoConfig config;
    if(!input_geometry(node,0,Rank::Vector,rows,columns) || !envelope_config(node,config) ||
       !tempo_window_frames(config.autocorrelation_seconds,config.sample_rate,config.hop_samples,lags) || !prior_contract(node,lags) ||
       !PreparedEnvelopeTempo::storage_bytes(rows,config,storage.bytes))return false;
    storage.count=config.estimator.aggregate==TempoAggregation::None?rows:1;storage.object=sizeof(EnvelopeTempo);return true;
}
}

domain::ProcessingSupport assess_tempo(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    TempoStorage storage;domain::ProcessingSupport support;
    if(n.kind=="TempoEstimate"){if(!estimator_storage(n,storage))return {};}
    else if(n.kind=="EnvelopeTempo"){if(!envelope_storage(n,storage))return {};}
    else return assess_tempo_ratio(n,p);
    const bool output=output_geometry(n,0,Rank::Vector,storage.count) || (storage.count==1 && output_geometry(n,0,Rank::Scalar,1));
    if(!prepared_storage::add_array(storage.bytes,storage.count,sizeof(float)) || !output)return {};
    descriptor_resources(n,p,storage.object,storage.bytes,support);return support;
}
}
