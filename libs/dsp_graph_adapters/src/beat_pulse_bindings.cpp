#include "operation_support.h"
#include <dsp_kernels/beat_tracker.h>
#include <algorithm>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
bool tracking_config(const dataflow_runtime::NumericOperationNodeView& n,BeatTrackerConfig& c) noexcept {
    return feature_rate(n,44100.f/512,c.frame_rate) && number(n,"tightness",100,c.tightness) &&
        integer(n,"maximum_period_frames",2048,c.maximum_period_frames) && flag(n,"trim",true,c.trim);
}
bool publish_tracking(const PreparedBeatTracker& tracker,std::span<NumericOperationOutput> out,domain::PortId first) noexcept {
    auto& links=*output_at(out,domain::PortId(first+2));const auto values=tracker.backlinks();
    if(!write_values(*output_at(out,first),tracker.local_scores()) || !write_values(*output_at(out,domain::PortId(first+1)),tracker.cumulative_scores()) ||
       !set_shape(links,values.size()))return false;
    for(std::size_t k=0;k<values.size();++k)if(store_element(links.value,{0,k,0},values[k])!=Status::ok)return false;
    return true;
}
class BeatPath final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<BeatPath>(*this);}
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n) {
        std::size_t a=0,b=0;BeatTrackerConfig c;
        if(!input_geometry(n,0,Rank::Vector,a,b) || !tracking_config(n,c) || !kernel_.prepare(a,c))return false;
        mask_.resize(a);return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+mask_.capacity();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto envelope=input_at(in,0)->contiguous_float32,bpm=input_at(in,1)->contiguous_float32;
        auto mask=std::span(mask_).first(envelope.size());
        const bool silent=bpm.size()==1 && bpm[0]==0 && std::all_of(envelope.begin(),envelope.end(),[](float v){return v==0;});
        const bool ready=silent?kernel_.silence(envelope.size(),mask):kernel_.process(envelope,bpm,mask);
        return ready && write_mask(*output_at(out,0),mask) && publish_tracking(kernel_,out,1);
    }
    PreparedBeatTracker kernel_;std::vector<std::uint8_t> mask_;
};
bool tracking_outputs(const dataflow_runtime::NumericOperationNodeView& n,std::size_t frames,domain::PortId first) noexcept {
    return output_geometry(n,first,Rank::Vector,frames) && output_geometry(n,domain::PortId(first+1),Rank::Vector,frames) &&
        output_geometry(n,domain::PortId(first+2),Rank::Vector,frames,1,DType::Int32);
}
domain::ProcessingSupport beat_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    std::size_t a=0,b=0,bpm=0,one=0,bytes=0;BeatTrackerConfig c;domain::ProcessingSupport s;
    const bool tempo=input_geometry(n,1,Rank::Vector,bpm,one) || input_geometry(n,1,Rank::Scalar,bpm,one);
    if(!input_geometry(n,0,Rank::Vector,a,b) || !tempo || (bpm!=1 && bpm!=a) ||
       !tracking_config(n,c) || !PreparedBeatTracker::storage_bytes(a,c,bytes))return {};
    bytes-=sizeof(PreparedBeatTracker);
    if(!prepared_storage::add_array(bytes,a,sizeof(std::uint8_t)) || !output_geometry(n,0,Rank::Vector,a,1,DType::UInt8) || !tracking_outputs(n,a,1))return {};
    descriptor_resources(n,p,sizeof(BeatPath),bytes,s);return s;
}
}
PreparedSequence prepare_beat_pulse(const dataflow_runtime::NumericOperationNode& node) {
    const auto n=definition(node);
    if(n.kind=="DynamicBeatPath"){auto op=std::make_unique<BeatPath>();return op->prepare(n)?std::move(op):nullptr;}
    return {};
}
domain::ProcessingSupport assess_beat_pulse(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    if(n.kind=="DynamicBeatPath")return beat_support(n,p);
    return {};
}
}
