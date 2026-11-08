#include "operation_support.h"
#include <domain/numeric/atomic_operation_ports.h>
#include <dsp_kernels/local_interval_beat_search.h>
#include <dsp_kernels/interval_sequence_search.h>
#include <algorithm>
#include <cmath>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
namespace decoder_ports=domain::atomic_ports::decoder;
struct SearchConfig {
    std::size_t low=48,high=300,ahead=2000,candidates=5;
    float rate=200,side=.2f,sigma=.18f;bool local=false;
};
bool search_config(const dataflow_runtime::NumericOperationNodeView& n,SearchConfig& c) noexcept {
    return integer(n,"minimum_interval",48,c.low) && c.low && integer(n,"maximum_interval",300,c.high) && c.high>=c.low &&
        integer(n,"look_ahead_frames",2000,c.ahead) && c.ahead && integer(n,"num_candidates",5,c.candidates) && c.candidates &&
        number(n,"frame_rate",200,c.rate) && c.rate>0 && number(n,"look_aside",.2f,c.side) &&
        number(n,"interval_sigma",.18f,c.sigma) && flag(n,"variable_tempo",false,c.local);
}
bool read_interval(const NumericOperationInput& input,std::size_t index,std::size_t& output) noexcept {
    if(input.value.contract->dtype==DType::UInt64){std::uint64_t value=0;
        if(load_element(input.value,{0,index,0},value)!=Status::ok || value>SIZE_MAX)return false;
        output=std::size_t(value);return true;
    }
    float value=0;
    if(load_element(input.value,{0,index,0},value)!=Status::ok || !std::isfinite(value) || value<1 || value>=float(SIZE_MAX) || std::floor(value)!=value)return false;
    output=std::size_t(value);return true;
}
bool read_intervals(const NumericOperationInput& input,std::span<std::size_t> output,std::size_t& count) noexcept {
    count=input.value.actual[0];if(count>output.size())return false;
    for(std::size_t i=0;i<count;++i)if(!read_interval(input,i,output[i]))return false;
    return true;
}
bool publish_events(std::span<NumericOperationOutput> out,std::span<const std::size_t> positions,float rate) noexcept {
    if(!set_shape(out[decoder_ports::times],positions.size()) || !write_indices(out[decoder_ports::positions],positions))return false;
    for(std::size_t i=0;i<positions.size();++i)if(store_element(out[decoder_ports::times].value,{0,i,0},float(positions[i])/rate)!=Status::ok)return false;
    return true;
}
class BeatTracking final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<BeatTracking>(*this);}
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n) {
        std::size_t a=0,b=0;if(!input_geometry(n,0,Rank::Vector,a,b) || !search_config(n,config_))return false;
        intervals_.resize(config_.local?a:1);
        return config_.local?local_.prepare(a,config_.ahead,config_.low,config_.high,config_.side):global_.prepare(a,config_.high,config_.side);
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return local_.workspace_bytes()+global_.workspace_bytes()+intervals_.capacity()*sizeof(std::size_t);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto values=input_at(in,decoder_ports::activations)->contiguous_float32;std::size_t count=0;
        if(!read_intervals(*input_at(in,decoder_ports::intervals),intervals_,count))return false;
        const auto intervals=std::span(intervals_).first(count);
        if(config_.local){
            if(!local_.process(values,intervals))return false;
            return publish_events(out,local_.positions(),config_.rate);
        }
        if(count!=1 || intervals[0]<config_.low || intervals[0]>config_.high || !global_.process(values,intervals[0]))return false;
        return publish_events(out,global_.positions(),config_.rate);
    }
    SearchConfig config_;PreparedLocalIntervalBeatSearch local_;PreparedIntervalBeatSearch global_;std::vector<std::size_t> intervals_;
};
class Crf final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Crf>(*this);}
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n) {
        std::size_t a=0,b=0;if(!input_geometry(n,0,Rank::Vector,a,b) || !search_config(n,config_))return false;
        intervals_.resize(config_.candidates);return kernel_.prepare(std::max(a,config_.high),config_.low,config_.high,config_.candidates,config_.sigma);
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+intervals_.capacity()*sizeof(std::size_t);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::size_t count=0;if(!read_intervals(*input_at(in,decoder_ports::intervals),intervals_,count))return false;
        if(!count)return publish_events(out,{},config_.rate) && write_indices(out[decoder_ports::trials],{}) && write_values(out[decoder_ports::scores],{});
        if(!kernel_.process(input_at(in,decoder_ports::activations)->contiguous_float32,std::span(intervals_).first(count)) && kernel_.outcome()!=SequenceDecodeOutcome::NoPath)return false;
        return publish_events(out,kernel_.positions(),config_.rate) && write_indices(out[decoder_ports::trials],kernel_.trial_intervals()) && write_values(out[decoder_ports::scores],kernel_.normalized_scores());
    }
    SearchConfig config_;PreparedIntervalSequenceSearch kernel_;std::vector<std::size_t> intervals_;
};
bool interval_input(const dataflow_runtime::NumericOperationNodeView& n,std::size_t count) noexcept {
    const auto c=contract(n,domain::PortDirection::Input,decoder_ports::intervals);
    return c && c->shape.rank==Rank::Vector && (c->dtype==DType::Float32 || c->dtype==DType::UInt64) && c->shape.extents[0].maximum==count;
}
domain::ProcessingSupport tracking_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0;SearchConfig c;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !search_config(n,c) || !interval_input(n,c.local?a:1))return {};
    const bool ok=c.local?PreparedLocalIntervalBeatSearch::storage_bytes(a,c.ahead,c.low,c.high,c.side,bytes):PreparedIntervalBeatSearch::storage_bytes(a,c.high,c.side,bytes);
    if(!ok || !prepared_storage::add_array(bytes,c.local?a:1,sizeof(std::size_t)) || !output_geometry(n,0,Rank::Vector,a+1) || !output_geometry(n,1,Rank::Vector,a+1,1,DType::UInt64))return {};
    descriptor_resources(n,p,sizeof(BeatTracking),bytes,s);return s;
}
domain::ProcessingSupport crf_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0;SearchConfig c;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !search_config(n,c) || !interval_input(n,c.candidates) ||
       !PreparedIntervalSequenceSearch::storage_bytes(std::max(a,c.high),c.low,c.high,c.candidates,c.sigma,bytes))return {};
    const auto count=std::max<std::size_t>(1,a/c.low);
    if(!prepared_storage::add_array(bytes,c.candidates,sizeof(std::size_t)) || !output_geometry(n,0,Rank::Vector,count) || !output_geometry(n,1,Rank::Vector,count,1,DType::UInt64) ||
       !output_geometry(n,2,Rank::Vector,c.candidates,1,DType::UInt64) || !output_geometry(n,3,Rank::Vector,c.candidates))return {};
    descriptor_resources(n,p,sizeof(Crf),bytes,s);return s;
}

}
PreparedSequence prepare_histogram(const dataflow_runtime::NumericOperationNode& node) {
    const auto n=definition(node);
    if(n.kind=="BeatTrackingSequence"){auto op=std::make_unique<BeatTracking>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="CrfBeatSequence"){auto op=std::make_unique<Crf>();return op->prepare(n)?std::move(op):nullptr;}
    return {};
}
domain::ProcessingSupport assess_histogram(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    if(n.kind=="BeatTrackingSequence")return tracking_support(n,p);
    if(n.kind=="CrfBeatSequence")return crf_support(n,p);
    return {};
}
}
