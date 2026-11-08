#include "common_support.h"
#include <domain/numeric/array_math.h>
#include <cmath>
namespace dsp_graph_adapters::detail {
namespace {
using namespace signal_contracts;
bool supported(const dataflow_runtime::NumericOperationNodeView& node) noexcept {
    const auto input=contract(node,domain::PortDirection::Input,domain::PortId(domain::AbsoluteValueInput::Values)),
        output=contract(node,domain::PortDirection::Output,domain::PortId(domain::AbsoluteValueOutput::AbsoluteValues));
    if(node.inputs.size()!=1 || node.outputs.size()!=1 || !input || !output || input->plane_count!=1 || output->plane_count!=1)return false;
    return (input->dtype==DType::Float32 || input->dtype==DType::Float64) && input->dtype==output->dtype &&
        input->shape==output->shape && input->representation.encoding==StorageEncoding::Native &&
        input->representation.layout==ChannelLayout::Planar && output->representation.encoding==StorageEncoding::Native &&
        output->representation.layout==ChannelLayout::Planar && output->representation.normalization==Normalization::None;
}
template<class Value> bool transform(const NumericBufferView& input,MutableNumericBufferView& output) noexcept {
    output.actual=input.actual;
    for(std::size_t row=0;row<input.actual[0];++row)for(std::size_t column=0;column<input.actual[1];++column){
        Value value=0;const ElementIndex index{0,row,column};
        if(load_element(input,index,value)!=Status::ok || store_element(output,index,std::abs(value))!=Status::ok)return false;
    }
    return true;
}
class Absolute final:public CommonKernel<Absolute> {
public:
    std::size_t workspace_bytes() const noexcept override {return 0;}
private:
    bool run(std::span<const NumericOperationInput> input,std::span<NumericOperationOutput> output) noexcept override {
        return input[0].value.contract->dtype==DType::Float64?transform<double>(input[0].value,output[0].value):transform<float>(input[0].value,output[0].value);
    }
};
}
PreparedCommon prepare_absolute_value(const dataflow_runtime::NumericOperationNodeView& node) {
    return supported(node)?std::make_unique<Absolute>():PreparedCommon{};
}
domain::ProcessingSupport assess_absolute_value(const dataflow_runtime::NumericOperationNodeView& node,const dataflow_runtime::NumericOperationProfile& profile) noexcept {
    domain::ProcessingSupport support;
    return supported(node) && common_resources(node,profile,sizeof(Absolute),0,support)?support:domain::ProcessingSupport{};
}
}
