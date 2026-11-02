#include "operation_support.h"
#include <dsp_kernels/state_beat_sequence.h>
#include <dsp_kernels/downbeat_state_decoder.h>
#include <dsp_kernels/bar_state_decoder.h>
#include <cmath>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
bool beat_config(const domain::ResolvedProcessingNode& n,BeatStateDecoderConfig& c) noexcept {
    return number(n,"frame_rate",100,c.frame_rate) && number(n,"minimum_bpm",55,c.min_bpm) && number(n,"maximum_bpm",215,c.max_bpm) &&
        number(n,"transition_lambda",100,c.transition_lambda) && number(n,"observation_lambda",16,c.observation_lambda) &&
        number(n,"transition_floor",2.220446049250313e-16f,c.transition_floor) && number(n,"activation_threshold",0,c.activation_threshold) &&
        integer(n,"num_intervals",0,c.num_intervals) && flag(n,"correct",true,c.correct) && flag(n,"fixed_interval",false,c.fixed_interval);
}
bool downbeat_config(const domain::ResolvedProcessingNode& n,DownbeatStateDecoderConfig& c) noexcept {
    return number(n,"frame_rate",100,c.frame_rate) && number(n,"observation_lambda",16,c.observation_lambda) &&
        number(n,"activation_threshold",.05f,c.activation_threshold) && flag(n,"correct",true,c.correct);
}
bool bar_config(const domain::ResolvedProcessingNode& n,BarStateDecoderConfig& c) noexcept {
    return number(n,"observation_weight",100,c.observation_weight) && number(n,"meter_change_probability",1e-7f,c.meter_change_probability);
}
bool index_scalar(NumericOperationOutput& o,std::size_t value) noexcept {
    return set_shape(o,1) && store_element(o.value,{0,0,0},std::uint64_t(value))==Status::ok;
}
bool probabilities(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float v){return std::isfinite(v) && v>=0 && v<=1;});
}
class BeatState final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<BeatState>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;BeatStateDecoderConfig c;bool online=false;
        return input_geometry(n,0,Rank::Vector,a,b) && beat_config(n,c) && flag(n,"online",false,online) &&
            flag(n,"reset_state",true,reset_) && kernel_.prepare(a,c,online);
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto values=input_at(in,0)->contiguous_float32;
        if(!probabilities(values) || !kernel_.process(values,reset_))return false;
        return write_values(*output_at(out,0),kernel_.events()) && write_u32(*output_at(out,1),kernel_.decoder().state_path()) &&
            write_scalar(*output_at(out,2),kernel_.online_bpm()) && index_scalar(*output_at(out,3),kernel_.decoder().state_path_first_frame());
    }
    PreparedStateBeatSequence kernel_;bool reset_=true;
};
class Downbeat final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Downbeat>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n,const ClassicalModel& model) {
        std::size_t a=0,b=0;DownbeatStateDecoderConfig c;
        if(!input_geometry(n,0,Rank::Matrix,a,b) || b!=2 || !downbeat_config(n,c) || !kernel_.prepare(a,model.downbeat_patterns,c))return false;
        return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& source=*input_at(in,0);const auto count=source.value.actual[0];const auto values=source.contiguous_float32;
        if(source.value.actual[1]!=2 || values.size()/2!=count || values.size()%2)return false;
        for(std::size_t k=0;k<count;++k){
            const float a=values[2*k],b=values[2*k+1];if(a<0 || b<0 || a+b>1)return false;
        }
        if(!kernel_.process(values))return false;
        auto& selected=*output_at(out,4);const auto id=kernel_.selected_model();
        const bool selection=id==SIZE_MAX?set_shape(selected,0):index_scalar(selected,id);
        return selection && write_values(*output_at(out,0),kernel_.beat_times()) && write_u32(*output_at(out,1),kernel_.beat_numbers()) &&
            write_values(*output_at(out,2),kernel_.model_likelihoods()) && write_u32(*output_at(out,3),kernel_.state_path()) &&
            index_scalar(*output_at(out,5),kernel_.state_path_first_frame());
    }
    PreparedDownbeatStateDecoder kernel_;
};
class Bar final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Bar>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n,const ClassicalModel& model) {
        std::size_t a=0,b=0;BarStateDecoderConfig c;
        return input_geometry(n,0,Rank::Vector,a,b) && bar_config(n,c) && kernel_.prepare(a,model.meters,c,model.pattern_switches);
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto beats=input_at(in,0)->contiguous_float32,probability=input_at(in,1)->contiguous_float32;
        if(beats.size()!=probability.size() || (!probability.empty() && !probabilities(probability.first(probability.size()-1))))return false;
        return kernel_.process(beats,probability) && write_u32(*output_at(out,0),kernel_.beat_numbers()) && write_u32(*output_at(out,1),kernel_.state_path());
    }
    PreparedBarStateDecoder kernel_;
};
domain::ProcessingSupport beat_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0;BeatStateDecoderConfig c;domain::ProcessingSupport s;bool online=false,reset=true;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !beat_config(n,c) || !flag(n,"online",false,online) || !flag(n,"reset_state",true,reset) ||
       !PreparedStateBeatSequence::storage_bytes(a,c,bytes) || !output_geometry(n,0,Rank::Vector,a) ||
       !output_geometry(n,1,Rank::Vector,a,1,DType::UInt32) || !output_geometry(n,2,Rank::Scalar,1) ||
       !output_geometry(n,3,Rank::Scalar,1,1,DType::UInt64))return {};
    descriptor_resources(n,p,sizeof(BeatState),bytes,s);return s;
}
domain::ProcessingSupport downbeat_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p,const ClassicalModel& model) noexcept {
    std::size_t a=0,b=0,bytes=0;DownbeatStateDecoderConfig c;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Matrix,a,b) || b!=2 || !downbeat_config(n,c) ||
       !PreparedDownbeatStateDecoder::storage_bytes(a,model.downbeat_patterns,c,bytes) ||
       !output_geometry(n,0,Rank::Vector,a) || !output_geometry(n,1,Rank::Vector,a,1,DType::UInt32) ||
       !output_geometry(n,2,Rank::Vector,model.downbeat_patterns.size()) || !output_geometry(n,3,Rank::Vector,a,1,DType::UInt32) ||
       !output_geometry(n,4,Rank::Vector,1,1,DType::UInt64) || !output_geometry(n,5,Rank::Scalar,1,1,DType::UInt64))return {};
    descriptor_resources(n,p,sizeof(Downbeat),bytes,s);return s;
}
domain::ProcessingSupport bar_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p,const ClassicalModel& model) noexcept {
    std::size_t a=0,b=0,x=0,y=0,bytes=0;BarStateDecoderConfig c;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !input_geometry(n,1,Rank::Vector,x,y) || a!=x || !bar_config(n,c) ||
       !PreparedBarStateDecoder::storage_bytes(a,model.meters,c,model.pattern_switches,bytes) ||
       !output_geometry(n,0,Rank::Vector,a,1,DType::UInt32) || !output_geometry(n,1,Rank::Vector,std::max(std::size_t{1},a-1),1,DType::UInt32))return {};
    descriptor_resources(n,p,sizeof(Bar),bytes,s);return s;
}
}
PreparedSequence prepare_state(const domain::ConfiguredProcessingNode& node,std::shared_ptr<const ClassicalModel> model) {
    const auto n=definition(node);
    if(n.kind=="BeatStateSequence"){auto op=std::make_unique<BeatState>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="DownbeatStateSequence" && model){auto op=std::make_unique<Downbeat>();return op->prepare(n,*model)?std::move(op):nullptr;}
    if(n.kind=="BarStateSequence" && model){auto op=std::make_unique<Bar>();return op->prepare(n,*model)?std::move(op):nullptr;}
    return {};
}
domain::ProcessingSupport assess_state(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p,const ClassicalModel* model) noexcept {
    if(n.kind=="BeatStateSequence")return beat_support(n,p);
    if(n.kind=="DownbeatStateSequence" && model)return downbeat_support(n,p,*model);
    if(n.kind=="BarStateSequence" && model)return bar_support(n,p,*model);
    return {};
}
}
