#include "operation_support.h"
#include <dsp_kernels/beat_feature_synchronization.h>
#include <dsp_kernels/model_prediction_selection.h>
#include <dsp_kernels/tempo_probability_histogram.h>
#include <dsp_kernels/pattern_state_decoder.h>
#include <algorithm>
#include <utility>

namespace dsp_graph_adapters {
namespace {
using dsp_kernels::prepared_storage::add_array;
bool mixture_storage(const dsp_kernels::GaussianMixtureData& mixture,std::size_t& bytes) noexcept {
    return add_array(bytes,mixture.weights.capacity(),sizeof(float)) && add_array(bytes,mixture.means.capacity(),sizeof(float)) &&
        add_array(bytes,mixture.covariances.capacity(),sizeof(float));
}
bool pattern_storage(const dsp_kernels::GmmPatternData& pattern,std::size_t& bytes) noexcept {
    if(!add_array(bytes,pattern.gmms.capacity(),sizeof(dsp_kernels::GaussianMixtureData)))return false;
    for(const auto& mixture:pattern.gmms)if(!mixture_storage(mixture,bytes))return false;
    return true;
}
bool model_arrays(const ClassicalModel& model,std::size_t& bytes) noexcept {
    return mixture_storage(model.mixture,bytes) && add_array(bytes,model.patterns.capacity(),sizeof(dsp_kernels::GmmPatternData)) &&
        add_array(bytes,model.tempi.capacity(),sizeof(dsp_kernels::PatternTempoParameters)) &&
        add_array(bytes,model.downbeat_patterns.capacity(),sizeof(dsp_kernels::DownbeatPatternConfig)) &&
        add_array(bytes,model.meters.capacity(),sizeof(std::uint32_t)) && add_array(bytes,model.pattern_switches.capacity(),sizeof(float));
}
}
bool ClassicalModel::owned_bytes(std::size_t& out) const noexcept {
    std::size_t bytes=sizeof(*this);if(!model_arrays(*this,bytes))return false;
    for(const auto& pattern:patterns)if(!pattern_storage(pattern,bytes))return false;
    for(const auto& tempo:tempi)if(!add_array(bytes,tempo.beat_transitions.capacity(),sizeof(dsp_kernels::TempoTransitionRule)))return false;
    out=bytes;return true;
}
namespace detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
bool pattern_outputs(const PreparedPatternStateDecoder& kernel,std::span<NumericOperationOutput> out) noexcept {
    return write_values(*output_at(out,0),kernel.beat_times()) && write_u32(*output_at(out,1),kernel.beat_numbers()) &&
        write_u32(*output_at(out,2),kernel.state_path()) && write_scalar(*output_at(out,3),kernel.log_probability());
}
class Pattern final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Pattern>(*this);}
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n,const ClassicalModel& model) {
        std::size_t a=0,b=0;float rate=50;
        return input_geometry(n,0,Rank::Matrix,a,b) && number(n,"frame_rate",50,rate) &&
            kernel_.prepare(a,model.patterns,model.tempi,rate) && b==kernel_.feature_count();
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& value=*input_at(in,0);
        return value.value.actual[1]==kernel_.feature_count() && kernel_.process(rows(value),value.value.actual[0]) && pattern_outputs(kernel_,out);
    }
    PreparedPatternStateDecoder kernel_;
};
bool pattern_shape(const dataflow_runtime::NumericOperationNodeView& n,std::size_t frames) noexcept {
    return output_geometry(n,0,Rank::Vector,frames) && output_geometry(n,1,Rank::Vector,frames,1,DType::UInt32) &&
        output_geometry(n,2,Rank::Vector,frames,1,DType::UInt32) && output_geometry(n,3,Rank::Scalar,1);
}
bool pattern_features_match(const ClassicalModel& model,std::size_t features) noexcept {
    for(const auto& pattern:model.patterns)for(const auto& mixture:pattern.gmms)if(mixture.features!=features)return false;
    return true;
}
domain::ProcessingSupport pattern_state_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p,const ClassicalModel* model) noexcept {
    std::size_t a=0,b=0,bytes=0;float rate=50;domain::ProcessingSupport support;
    if(!model || !input_geometry(n,0,Rank::Matrix,a,b) || !number(n,"frame_rate",50,rate) ||
       !PreparedPatternStateDecoder::storage_bytes(a,model->patterns,model->tempi,rate,bytes) || !pattern_shape(n,a) ||
       !pattern_features_match(*model,b))return {};
    descriptor_resources(n,p,sizeof(Pattern),bytes,support);return support;
}
class Mixture final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Mixture>(*this);}
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n,const ClassicalModel& model) {
        float retry=0;if(!number(n,"diagonal_retry",1e-7f,retry) || !kernel_.prepare(model.mixture.parameters(),retry))return false;
        sample_.resize(kernel_.features());return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+sample_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& value=*input_at(in,0);const auto count=value.value.actual[0];
        if(value.value.actual[1]!=sample_.size() || !set_shape(*output_at(out,0),count) ||
           !set_shape(*output_at(out,1),count,kernel_.components()) || !set_shape(*output_at(out,2),count,kernel_.components()))return false;
        for(std::size_t t=0;t<count;++t){
            float probability=0;if(!read_row(&value,t,sample_) || !kernel_.process(sample_,probability))return false;
            if(store_element(output_at(out,0)->value,{0,t,0},probability)!=Status::ok ||
               !write_row(output_at(out,1),t,kernel_.component_log_probabilities()) || !write_row(output_at(out,2),t,kernel_.responsibilities()))return false;
        }
        return true;
    }
    PreparedGaussianMixture kernel_;std::vector<float> sample_;
};
class PredictionSelection final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<PredictionSelection>(*this);}
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n) {
        std::size_t a=0,b=0;return integer(n,"reference_count",0,references_) && input_geometry(n,0,Rank::Matrix,a,b) && kernel_.prepare(a,b);
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& value=*input_at(in,0);
        if(!kernel_.process(rows(value),value.value.actual[0],value.value.actual[1],references_) ||
           !write_values(*output_at(out,0),kernel_.selected_values()) || !write_values(*output_at(out,1),kernel_.reference()) ||
           !write_values(*output_at(out,2),kernel_.candidate_errors()))return false;
        auto& selected=*output_at(out,3);const auto id=kernel_.selected_model();
        return id==SIZE_MAX?set_shape(selected,0):set_shape(selected,1) && store_element(selected.value,{0,0,0},std::uint64_t(id))==Status::ok;
    }
    PreparedModelPredictionSelection kernel_;std::size_t references_=0;
};
class ProbabilityHistogram final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<ProbabilityHistogram>(*this);}
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n) noexcept {
        return number(n,"minimum_bpm",40,minimum_) && number(n,"maximum_bpm",250,maximum_) && minimum_<=maximum_;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return 0;}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto values=input_at(in,0)->contiguous_float32;TempoProbabilityRange range;
        if(!tempo_probability_range(values,minimum_,maximum_,range) || !write_values(*output_at(out,0),values.subspan(range.first,range.count)) ||
           !set_shape(*output_at(out,1),range.count))return false;
        for(std::size_t k=0;k<range.count;++k)if(store_element(output_at(out,1)->value,{0,k,0},float(range.first+k))!=Status::ok)return false;
        return true;
    }
    float minimum_=40,maximum_=250;
};
class Synchronization final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Synchronization>(*this);}
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n) {
        std::size_t a=0,b=0,empty=0;BeatFeatureSynchronizationConfig c;
        if(!input_geometry(n,0,Rank::Matrix,a,b) || !integer(n,"subdivisions",4,c.subdivisions) ||
           !number(n,"frame_rate",100,c.frame_rate) || !integer(n,"empty",0,empty) || empty>1)return false;
        c.empty=EmptySubdivision(empty);subdivisions_=c.subdivisions;return kernel_.prepare(b,c);
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& value=*input_at(in,0);const auto beats=input_at(in,1)->contiguous_float32;auto& target=*output_at(out,0);
        const auto maximum=beats.empty()?0:(beats.size()-1)*subdivisions_;
        if(!set_shape(target,maximum,value.value.actual[1]) || !kernel_.process(rows(value),value.value.actual[0],value.value.actual[1],beats,rows(target)))return false;
        const auto count=kernel_.retained_beats();
        if(!set_shape(target,count?(count-1)*subdivisions_:0,value.value.actual[1]))return false;
        return write_scalar(*output_at(out,1),float(count)) && write_scalar(*output_at(out,2),float(kernel_.empty_subdivisions()));
    }
    PreparedBeatFeatureSynchronization kernel_;std::size_t subdivisions_=4;
};
domain::ProcessingSupport mixture_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p,const ClassicalModel* model) noexcept {
    if(!model)return {};
    std::size_t a=0,b=0,bytes=0;float retry=0;domain::ProcessingSupport s;
    const auto parameters=model->mixture.parameters();
    if(!input_geometry(n,0,Rank::Matrix,a,b) || b!=parameters.features || !number(n,"diagonal_retry",1e-7f,retry) ||
       !PreparedGaussianMixture::storage_bytes(parameters,retry,bytes) || !prepared_storage::add_array(bytes,b,sizeof(float)) ||
       !output_geometry(n,0,Rank::Vector,a) || !output_geometry(n,1,Rank::Matrix,a,parameters.components) ||
       !output_geometry(n,2,Rank::Matrix,a,parameters.components))return {};
    descriptor_resources(n,p,sizeof(Mixture),bytes,s);return s;
}
domain::ProcessingSupport selection_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0,refs=0;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Matrix,a,b) || !integer(n,"reference_count",0,refs) ||
       !PreparedModelPredictionSelection::storage_bytes(a,b,bytes) || !output_geometry(n,0,Rank::Vector,b) ||
       !output_geometry(n,1,Rank::Vector,b) || !output_geometry(n,2,Rank::Vector,a) || !output_geometry(n,3,Rank::Vector,1,1,DType::UInt64))return {};
    descriptor_resources(n,p,sizeof(PredictionSelection),bytes,s);return s;
}
domain::ProcessingSupport probability_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    std::size_t a=0,b=0;float lo=0,hi=0;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !number(n,"minimum_bpm",40,lo) || !number(n,"maximum_bpm",250,hi) ||
       lo>hi || !output_geometry(n,0,Rank::Vector,a) || !output_geometry(n,1,Rank::Vector,a))return {};
    descriptor_resources(n,p,sizeof(ProbabilityHistogram),0,s);return s;
}
domain::ProcessingSupport synchronization_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    std::size_t a=0,b=0,beats=0,one=0,empty=0,bytes=0,count=0;BeatFeatureSynchronizationConfig c;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Matrix,a,b) || !input_geometry(n,1,Rank::Vector,beats,one) ||
       !integer(n,"subdivisions",4,c.subdivisions) || !number(n,"frame_rate",100,c.frame_rate) || !integer(n,"empty",0,empty) || empty>1)return {};
    c.empty=EmptySubdivision(empty);
    if(!prepared_storage::mul_bytes(beats-1,c.subdivisions,count) || !PreparedBeatFeatureSynchronization::storage_bytes(b,c,bytes) ||
       !output_geometry(n,0,Rank::Matrix,count,b) || !output_geometry(n,1,Rank::Scalar,1) || !output_geometry(n,2,Rank::Scalar,1))return {};
    descriptor_resources(n,p,sizeof(Synchronization),bytes,s);return s;
}
template<class Operation,class... Arguments> PreparedSequence prepare_model_kernel(const dataflow_runtime::NumericOperationNodeView& node,Arguments&&... arguments) {
    auto operation=std::make_unique<Operation>();
    return operation->prepare(node,std::forward<Arguments>(arguments)...)?std::move(operation):nullptr;
}

PreparedSequence prepare_supplied_model(const dataflow_runtime::NumericOperationNodeView& node,const ClassicalModel* model) {
    if(!model)return {};
    if(node.kind=="PatternStateSequence")return prepare_model_kernel<Pattern>(node,*model);
    if(node.kind=="GaussianMixtureScore")return prepare_model_kernel<Mixture>(node,*model);
    return {};
}
PreparedSequence prepare_feature_model(const dataflow_runtime::NumericOperationNodeView& node) {
    if(node.kind=="ModelPredictionSelection")return prepare_model_kernel<PredictionSelection>(node);
    if(node.kind=="TempoProbabilityHistogram")return prepare_model_kernel<ProbabilityHistogram>(node);
    if(node.kind=="BeatFeatureSynchronization")return prepare_model_kernel<Synchronization>(node);
    return {};
}
}
PreparedSequence prepare_model(const dataflow_runtime::NumericOperationNode& node,std::shared_ptr<const ClassicalModel> model) {
    const auto view=definition(node);
    if(view.kind=="PatternStateSequence" || view.kind=="GaussianMixtureScore")return prepare_supplied_model(view,model.get());
    return prepare_feature_model(view);
}

domain::ProcessingSupport assess_model(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p,const ClassicalModel* model) noexcept {
    if(n.kind=="PatternStateSequence")return pattern_state_support(n,p,model);
    if(n.kind=="GaussianMixtureScore")return mixture_support(n,p,model);
    if(n.kind=="ModelPredictionSelection")return selection_support(n,p);
    if(n.kind=="TempoProbabilityHistogram")return probability_support(n,p);
    if(n.kind=="BeatFeatureSynchronization")return synchronization_support(n,p);
    return {};
}
}
}
