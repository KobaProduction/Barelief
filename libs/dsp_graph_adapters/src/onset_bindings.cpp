#include "operation_support.h"
#include <dsp_kernels/onset_peaks.h>
namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
class Backtrack final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Backtrack>(*this);}
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n) {
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
domain::ProcessingSupport backtrack_support(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    const auto events=contract(n,domain::PortDirection::Input,0);std::size_t a=0,b=0,bytes=0;domain::ProcessingSupport s;
    if(!events || events->dtype!=DType::UInt64 || events->shape.rank!=Rank::Vector || events->plane_count!=1 ||
       events->shape.extents[0].maximum>SIZE_MAX || !input_geometry(n,1,Rank::Vector,a,b) ||
       !PreparedOnsetBacktracker::storage_bytes(a,bytes))return {};
    const auto count=std::size_t(events->shape.extents[0].maximum);
    if(!prepared_storage::add_array(bytes,count,2*sizeof(std::size_t)) || !output_geometry(n,0,Rank::Vector,count,1,DType::UInt64))return {};
    descriptor_resources(n,p,sizeof(Backtrack),bytes,s);return s;
}
}
PreparedSequence prepare_onset(const dataflow_runtime::NumericOperationNode& node) {
    const auto n=definition(node);
    if(n.kind!="OnsetBacktrack")return {};
    auto operation=std::make_unique<Backtrack>();return operation->prepare(n)?std::move(operation):nullptr;
}
domain::ProcessingSupport assess_onset(const dataflow_runtime::NumericOperationNodeView& node,const dataflow_runtime::NumericOperationProfile& profile) noexcept {
    return node.kind=="OnsetBacktrack"?backtrack_support(node,profile):domain::ProcessingSupport{};
}
}
