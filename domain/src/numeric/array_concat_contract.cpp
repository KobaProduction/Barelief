#include <domain/numeric/contracts.h>

namespace domain {
using namespace signal_contracts;
Status array_concat_output_contract(const ConfiguredProcessingNode& node,OwnedProcessingContract& result) noexcept {
    if(node.kind!="ArrayConcat" || node.inputs.size()!=2)return Status::invalid_argument;
    const auto a=node.inputs[0].contract.view(),b=node.inputs[1].contract.view();
    const auto* left=std::get_if<NumericContract>(&a);const auto* right=std::get_if<NumericContract>(&b);
    const auto supported=[](const NumericContract* c){return c && c->dtype==DType::Float32 && c->channel_order.size()==1 &&
        c->shape.rank==Rank::Vector && c->shape.extents[0].minimum && c->shape.extents[0].minimum==c->shape.extents[0].maximum &&
        c->representation.encoding==StorageEncoding::Native && c->representation.layout==ChannelLayout::Planar && c->representation.normalization==Normalization::None;};
    if(!supported(left) || !supported(right) || left->clock.domain!=right->clock.domain ||
       left->clock.sample_rate!=right->clock.sample_rate || left->clock.tick_seconds!=right->clock.tick_seconds)return Status::incompatible_contract;
    const auto count=left->shape.extents[0].maximum;
    if(right->shape.extents[0].maximum>UINT32_MAX || count>UINT32_MAX-right->shape.extents[0].maximum)return Status::exhausted;
    auto output=*left;output.shape.axes[0]=Axis::Band;output.shape.extents[0]={count+right->shape.extents[0].maximum,count+right->shape.extents[0].maximum};
    output.semantic=Semantic::Numeric;output.history_element=Semantic::Numeric;output.quantity=Quantity::Unspecified;output.coordinates={};
    if(left->unit!=right->unit)output.unit=NumericUnit::Values;
    return own_processing_contract(output,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,result);
}
}
