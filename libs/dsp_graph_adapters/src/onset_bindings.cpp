#include "operation_support.h"
#include <dsp_kernels/onset_detection_sequence.h>
#include <dsp_kernels/onset_strength_sequence.h>
#include <algorithm>
#include <cmath>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
bool peak_config(const domain::ResolvedProcessingNode& n,OnsetPeakConfig& c) noexcept {
    std::size_t selection=0;
    if(!integer(n,"pre_max",1,c.pre_max) || !integer(n,"post_max",1,c.post_max) ||
       !integer(n,"pre_average",1,c.pre_average) || !integer(n,"post_average",1,c.post_average) ||
       !integer(n,"wait",0,c.wait) || !number(n,"delta",.07f,c.delta) || !flag(n,"normalize",true,c.normalize) ||
       !integer(n,"selection",0,selection) || selection>2)return false;
    c.selection=PeakSelection(selection);return true;
}
bool detection_config(const domain::ResolvedProcessingNode& n,OnsetDetectionSequenceConfig& c) noexcept {
    bool custom=false;std::size_t units=0;
    if(!number(n,"sample_rate",44100,c.sample_rate) || !integer(n,"hop_samples",512,c.hop_samples) ||
       !flag(n,"sparse",true,c.sparse) || !flag(n,"backtrack",false,c.backtrack) ||
       !integer(n,"units",0,units) || units>2 || !flag(n,"custom_peaks",false,custom))return false;
    c.units=EventCoordinateUnit(units);
    if(custom){OnsetPeakConfig p;if(!peak_config(n,p))return false;c.peaks=p;}
    return true;
}
bool strength_config(const domain::ResolvedProcessingNode& n,OnsetStrengthSequenceConfig& c) noexcept {
    std::size_t aggregate=0;
    if(!integer(n,"lag",1,c.strength.lag) || !integer(n,"max_size",1,c.strength.max_size) ||
       !integer(n,"n_fft",2048,c.strength.n_fft) || !integer(n,"hop_samples",512,c.strength.hop_length) ||
       !flag(n,"center",true,c.strength.center) || !flag(n,"detrend",false,c.strength.detrend) ||
       !flag(n,"aggregate_features",true,c.aggregate_features) || !integer(n,"aggregate",0,aggregate) || aggregate>2)return false;
    c.strength.aggregate=OnsetAggregate(aggregate);return true;
}
bool grouping(const domain::ResolvedProcessingNode& n,std::size_t features,std::size_t& groups,std::size_t& indices) noexcept {
    const auto offsets=array(n,"group_offsets"),values=array(n,"group_indices");groups=0;indices=values.size();
    if(offsets.empty())return values.empty();
    if(offsets.size()<2 || offsets[0]!=0 || offsets.back()!=float(values.size()))return false;
    for(std::size_t k=1;k<offsets.size();++k)
        if(!std::isfinite(offsets[k]) || offsets[k]!=std::floor(offsets[k]) || offsets[k]<=offsets[k-1] || offsets[k]-offsets[k-1]>float(features))return false;
    for(float i:values)if(!std::isfinite(i) || i<0 || i>=float(features) || i!=std::floor(i))return false;
    groups=offsets.size()-1;return true;
}
bool optional_reference(const domain::ResolvedProcessingNode& n,Rank rank,std::size_t a,std::size_t b) noexcept {
    if(!contract(n,domain::PortDirection::Input,1))return true;
    std::size_t x=0,y=0;return input_geometry(n,1,rank,x,y) && x==a && y==b;
}
class Strength final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Strength>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0,groups=0,indices=0;OnsetStrengthSequenceConfig c;
        if(!input_geometry(n,0,Rank::Matrix,a,b) || !strength_config(n,c) || !grouping(n,b,groups,indices))return false;
        const auto offsets=array(n,"group_offsets"),values=array(n,"group_indices");
        std::vector<std::size_t> converted;converted.reserve(values.size());for(float v:values)converted.push_back(std::size_t(v));
        std::vector<OnsetFeatureGroup> views;views.reserve(groups);
        for(std::size_t k=0;k<groups;++k)views.push_back({std::span<const std::size_t>(converted).subspan(std::size_t(offsets[k]),std::size_t(offsets[k+1]-offsets[k]))});
        std::size_t bound=0,actual=0;
        if(!prepared_storage::add_array(bound,indices,sizeof(std::size_t)) || !prepared_storage::add_array(bound,groups,sizeof(OnsetFeatureGroup)) ||
           !prepared_storage::add_array(actual,converted.capacity(),sizeof(std::size_t)) || !prepared_storage::add_array(actual,views.capacity(),sizeof(OnsetFeatureGroup)) || actual>bound)return false;
        return kernel_.prepare(b,a,c,views);
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& value=*input_at(in,0);auto& target=*output_at(out,0);const auto* ref=available_input_at(in,1);
        if(ref && ref->value.actual!=value.value.actual)return false;
        return set_shape(target,kernel_.output_frames(value.value.actual[0]),kernel_.output_features()) &&
            kernel_.process(rows(value),value.value.actual[0],rows(target),ref?rows(*ref):FloatMatrixSource{});
    }
    PreparedOnsetStrengthSequence kernel_;
};
class Detection final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Detection>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;OnsetDetectionSequenceConfig c;
        if(!input_geometry(n,0,Rank::Vector,a,b) || !detection_config(n,c))return false;
        std::size_t energy=a;if(const auto e=contract(n,domain::PortDirection::Input,1))energy=std::size_t(e->shape.extents[0].maximum);
        return kernel_.prepare(a,c,energy);
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& value=*input_at(in,0);const auto* energy=available_input_at(in,1);OnsetDetectionResult result;
        if(!kernel_.process(value.contiguous_float32,result,energy?std::optional(energy->contiguous_float32):std::nullopt))return false;
        auto& target=*output_at(out,0);
        if(target.value.contract->dtype==DType::UInt8)return write_mask(target,result.dense);
        if(target.value.contract->dtype==DType::Float64)return write_seconds(target,result.seconds);
        if(!result.samples.empty())return write_samples(target,result.samples);
        return write_indices(target,result.frames);
    }
    PreparedOnsetDetectionSequence kernel_;
};
class Backtrack final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Backtrack>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        const auto events=contract(n,domain::PortDirection::Input,0);std::size_t a=0,b=0;
        if(!events || !input_geometry(n,1,Rank::Vector,a,b) || !kernel_.prepare(a))return false;
        source_.resize(std::size_t(events->shape.extents[0].maximum));result_.resize(source_.size());return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+(source_.capacity()+result_.capacity())*sizeof(std::size_t);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& events=*input_at(in,0);const auto& energy=*input_at(in,1);const auto count=events.value.actual[0];
        if(count>source_.size())return false;
        for(std::size_t k=0;k<count;++k){
            std::uint64_t v=0;if(load_element(events.value,{0,k,0},v)!=Status::ok || v>=energy.value.actual[0] || v>SIZE_MAX)return false;
            source_[k]=std::size_t(v);
        }
        auto result=std::span(result_).first(count);
        return kernel_.process(std::span(source_).first(count),energy.contiguous_float32,result) && write_indices(*output_at(out,0),result);
    }
    PreparedOnsetBacktracker kernel_;
    std::vector<std::size_t> source_,result_;
};
domain::ProcessingSupport strength_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,groups=0,indices=0,bytes=0;OnsetStrengthSequenceConfig c;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Matrix,a,b) || !optional_reference(n,Rank::Matrix,a,b) || !strength_config(n,c) ||
       !grouping(n,b,groups,indices) || !PreparedOnsetStrengthSequence::storage_bytes(b,a,c,groups,indices,bytes) ||
       !prepared_storage::add_array(bytes,indices,sizeof(std::size_t)) || !prepared_storage::add_array(bytes,groups,sizeof(OnsetFeatureGroup)) ||
       !output_geometry(n,0,Rank::Matrix,c.strength.center?a:std::max(a,c.strength.lag),c.aggregate_features?(groups?groups:1):b))return {};
    descriptor_resources(n,p,sizeof(Strength),bytes,s);return s;
}
domain::ProcessingSupport detection_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,energy=0,bytes=0;OnsetDetectionSequenceConfig c;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !detection_config(n,c))return {};
    energy=a;if(contract(n,domain::PortDirection::Input,1) && !input_geometry(n,1,Rank::Vector,energy,b))return {};
    const auto dtype=!c.sparse?DType::UInt8:c.units==EventCoordinateUnit::Seconds?DType::Float64:DType::UInt64;
    if(!PreparedOnsetDetectionSequence::storage_bytes(a,c,energy,bytes) || !output_geometry(n,0,Rank::Vector,a,1,dtype))return {};
    descriptor_resources(n,p,sizeof(Detection),bytes,s);return s;
}
domain::ProcessingSupport backtrack_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    const auto events=contract(n,domain::PortDirection::Input,0);std::size_t a=0,b=0,bytes=0;domain::ProcessingSupport s;
    if(!events || events->dtype!=DType::UInt64 || events->shape.rank!=Rank::Vector || events->channel_order.size()!=1 ||
       events->shape.extents[0].maximum>SIZE_MAX || !input_geometry(n,1,Rank::Vector,a,b) ||
       !PreparedOnsetBacktracker::storage_bytes(a,bytes))return {};
    const auto count=std::size_t(events->shape.extents[0].maximum);
    if(!prepared_storage::add_array(bytes,count,2*sizeof(std::size_t)) || !output_geometry(n,0,Rank::Vector,count,1,DType::UInt64))return {};
    descriptor_resources(n,p,sizeof(Backtrack),bytes,s);return s;
}
}
PreparedSequence prepare_onset(const domain::ConfiguredProcessingNode& node) {
    const auto n=definition(node);
    if(n.kind=="OnsetStrengthSequence"){auto v=std::make_unique<Strength>();return v->prepare(n)?std::move(v):nullptr;}
    if(n.kind=="OnsetDetection"){auto v=std::make_unique<Detection>();return v->prepare(n)?std::move(v):nullptr;}
    if(n.kind=="OnsetBacktrack"){auto v=std::make_unique<Backtrack>();return v->prepare(n)?std::move(v):nullptr;}
    return {};
}
domain::ProcessingSupport assess_onset(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    if(n.kind=="OnsetStrengthSequence")return strength_support(n,p);
    if(n.kind=="OnsetDetection")return detection_support(n,p);
    if(n.kind=="OnsetBacktrack")return backtrack_support(n,p);
    return {};
}
}
