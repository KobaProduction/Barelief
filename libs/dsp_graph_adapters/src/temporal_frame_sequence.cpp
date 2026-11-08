#include "common_support.h"
#include <dsp_kernels/temporal_window.h>

namespace dsp_graph_adapters::detail {
namespace {
using namespace signal_contracts;using namespace dsp_kernels;
bool framing(const dataflow_runtime::NumericOperationNodeView& node,TemporalWindowConfig& config) noexcept {
    std::size_t tail=1;bool center=false;
    if(!integer(node,"window_length",2048,config.length) || !integer(node,"hop",512,config.hop) ||
       !integer(node,"tail",1,tail) || tail>1 || !flag(node,"center",false,center) || center)return false;
    config.function=WindowFunction::Rectangular;config.periodic=false;config.center=false;config.tail=TemporalTail(tail);
    return config.length>=2 && config.length<=65536 && config.hop>0;
}
class FrameSequence final:public CommonKernel<FrameSequence> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& node) {
        TemporalWindowConfig config;std::size_t count=0;
        if(!common_vector(node,0,count) || !framing(node,config) || !window_.prepare(config))return false;
        input_.resize(count);row_.resize(config.length);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return window_.workspace_bytes()+(input_.capacity()+row_.capacity())*sizeof(float);}
private:
    bool write_frame(std::size_t row,NumericOperationOutput& output,std::span<const float> input) noexcept {
        if(!window_.read(input,row,row_))return false;
        for(std::size_t i=0;i<row_.size();++i)if(store_element(output.value,{0,row,i},row_[i])!=Status::ok)return false;
        return true;
    }
    bool run(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) noexcept override {
        std::span<const float> input;if(!common_values(inputs[0],input_,input))return false;
        auto& output=outputs[0];const auto count=window_.frames(input.size());output.value.actual={count,row_.size()};
        for(std::size_t row=0;row<count;++row)if(!write_frame(row,output,input))return false;
        return true;
    }
    PreparedTemporalWindow window_;std::vector<float> input_,row_;
};
}
bool temporal_frame_sequence_rows(const dataflow_runtime::NumericOperationNodeView& node,std::size_t samples,std::size_t& rows) noexcept {
    dsp_kernels::TemporalWindowConfig config;
    return framing(node,config) && dsp_kernels::PreparedTemporalWindow::frame_count(config,samples,rows);
}
PreparedCommon prepare_temporal_frame_sequence(const dataflow_runtime::NumericOperationNodeView& node) {
    auto next=std::make_unique<FrameSequence>();return next->prepare(node)?std::move(next):nullptr;
}
domain::ProcessingSupport assess_temporal_frame_sequence(const dataflow_runtime::NumericOperationNodeView& node,const dataflow_runtime::NumericOperationProfile& profile) noexcept {
    TemporalWindowConfig config;std::size_t count=0,frames=0,bytes=0;domain::ProcessingSupport result;
    if(!common_vector(node,0,count) || !framing(node,config) || !PreparedTemporalWindow::frame_count(config,count,frames) ||
       !common_output(node,0,Rank::Matrix,frames,config.length) || !PreparedTemporalWindow::storage_bytes(config,{},bytes) ||
       !prepared_storage::add_array(bytes,count,sizeof(float)) || !prepared_storage::add_array(bytes,config.length,sizeof(float)))return {};
    return common_resources(node,profile,sizeof(FrameSequence),bytes,result)?result:domain::ProcessingSupport{};
}
}
