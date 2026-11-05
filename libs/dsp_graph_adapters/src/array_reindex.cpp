#include "common_support.h"
#include <cmath>

namespace dsp_graph_adapters::detail {
namespace {
using namespace signal_contracts;
bool indices_valid(const domain::ResolvedProcessingNode& node,std::size_t& count) noexcept {
    const auto source=contract(node,domain::PortDirection::Input,0);
    if(!source || source->dtype!=DType::Float32 || source->channel_order.size()!=1 ||
       (source->shape.rank!=Rank::Vector && source->shape.rank!=Rank::Matrix))return false;
    const auto axis=source->shape.rank==Rank::Matrix?1u:0u;
    count=std::size_t(source->shape.extents[axis].maximum);
    if(!count || source->shape.extents[axis].minimum!=count)return false;
    const auto indices=array(node,"indices");
    if(indices.empty())return false;
    for(float index:indices)
        if(!std::isfinite(index) || index<0 || double(index)>=double(count) || std::floor(index)!=index)return false;
    return true;
}
class Reindex final:public CommonKernel<Reindex> {
public:
    bool prepare(const domain::ResolvedProcessingNode& node) {
        std::size_t count=0;if(!indices_valid(node,count))return false;
        const auto indices=array(node,"indices");indices_.assign(indices.begin(),indices.end());
        return true;
    }
    std::size_t workspace_bytes() const noexcept override {return indices_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) noexcept override {
        auto& output=outputs[0];const auto matrix=inputs[0].value.contract->shape.rank==Rank::Matrix;
        output.value.actual=matrix?std::array<std::size_t,2>{inputs[0].value.actual[0],indices_.size()}:std::array<std::size_t,2>{indices_.size(),1};
        const auto rows=matrix?inputs[0].value.actual[0]:1;
        for(std::size_t row=0;row<rows;++row)if(!copy_row(inputs[0].value,output.value,row,matrix))return false;
        return true;
    }
    bool copy_row(const NumericView& input,MutableNumericView& output,std::size_t row,bool matrix) const noexcept {
        for(std::size_t i=0;i<indices_.size();++i){
            float value=0;const auto source=matrix?ElementIndex{0,row,std::size_t(indices_[i])}:ElementIndex{0,std::size_t(indices_[i]),0};
            const auto target=matrix?ElementIndex{0,row,i}:ElementIndex{0,i,0};
            if(load_element(input,source,value)!=Status::ok || store_element(output,target,value)!=Status::ok)return false;
        }
        return true;
    }
    std::vector<float> indices_;
};
}
PreparedCommon prepare_array_reindex(const domain::ResolvedProcessingNode& node) {
    auto next=std::make_unique<Reindex>();
    return next->prepare(node)?std::move(next):nullptr;
}
domain::ProcessingSupport assess_array_reindex(const domain::ResolvedProcessingNode& node,const dataflow_runtime::NumericRuntimeProfile& profile) noexcept {
    std::size_t count=0,bytes=0;domain::ProcessingSupport result;
    const auto indices=array(node,"indices");
    if(!indices_valid(node,count))return {};
    const auto source=contract(node,domain::PortDirection::Input,0);const auto matrix=source->shape.rank==Rank::Matrix;
    if(!common_output(node,0,source->shape.rank,matrix?std::size_t(source->shape.extents[0].maximum):indices.size(),matrix?indices.size():1) ||
       !dataflow_runtime::numeric_operation_mul_bytes(indices.size(),sizeof(float),bytes))return {};
    return common_resources(node,profile,sizeof(Reindex),bytes,result)?result:domain::ProcessingSupport{};
}
}
