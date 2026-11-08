#include <domain/numeric/contracts.h>
#include <domain/numeric/array_concat.h>
namespace domain {
using namespace signal_contracts;
std::optional<ArrayConcatAxis> array_concat_axis(const ResolvedProcessingNode& node) noexcept {
    ProcessingParameterView field;
    if(resolve_processing_parameter(node,"axis",field)!=Status::ok)return {};
    const auto* value=std::get_if<NumericLiteral>(&field);if(!value)return {};
    return std::visit([](auto number){return array_concat_axis(double(number));},*value);
}
namespace {
bool concat_supported(const NumericContract* c) {
    if(!c || c->dtype!=DType::Float32 || c->channel_order.size()!=1 ||
       c->representation.encoding!=StorageEncoding::Native || c->representation.layout!=ChannelLayout::Planar ||
       c->representation.normalization!=Normalization::None)return false;
    for(std::size_t axis=0;axis<(c->shape.rank==Rank::Matrix?2u:1u);++axis){
        const auto extent=c->shape.extents[axis];
        if(!extent.maximum || extent.minimum>extent.maximum || extent.maximum>UINT32_MAX)return false;
    }
    return true;
}
bool concat_geometry(const NumericContract& left,const NumericContract& right,std::size_t axis) noexcept {
    const bool matrix=left.shape.rank==Rank::Matrix;
    if(matrix!=(right.shape.rank==Rank::Matrix) || (axis && !matrix))return false;
    if(!matrix)return true;
    const auto other=1-axis;
    return left.shape.extents[other]==right.shape.extents[other] && left.shape.axes[other]==right.shape.axes[other] &&
        left.coordinates[other]==right.coordinates[other];
}
Status concatenate_contracts(const NumericContract& left,const NumericContract& right,std::size_t axis,OwnedProcessingContract& result) {
    const auto a=left.shape.extents[axis],b=right.shape.extents[axis];
    if(a.maximum>UINT32_MAX-b.maximum)return Status::exhausted;
    auto output=left;
    if(output.shape.rank!=Rank::Matrix)output.shape.rank=Rank::Vector;
    output.shape.axes[axis]=Axis::Declared;output.shape.extents[axis]={a.minimum+b.minimum,a.maximum+b.maximum};
    output.semantic=Semantic::Numeric;output.history_element=Semantic::Numeric;output.quantity=Quantity::Unspecified;output.coordinates[axis]={};
    if(left.unit!=right.unit)output.unit=NumericUnit::Values;
    return own_processing_contract(output,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,result);
}
}
Status array_concat_output_contract(const ConfiguredProcessingNode& node,OwnedProcessingContract& result) noexcept {
    if(node.kind!="ArrayConcat" || node.inputs.size()!=2)return Status::invalid_argument;
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ArrayConcat,node.inputs,node.outputs,&node.parameters,node.enabled};
    const auto axis=array_concat_axis(resolved);if(!axis)return Status::invalid_argument;
    const auto a=node.inputs[0].contract.view(),b=node.inputs[1].contract.view();
    const auto* left=std::get_if<NumericContract>(&a);const auto* right=std::get_if<NumericContract>(&b);
    if(!concat_supported(left) || !concat_supported(right) || !concat_geometry(*left,*right,std::size_t(*axis)) ||
       left->clock.domain!=right->clock.domain || left->clock.sample_rate!=right->clock.sample_rate ||
       left->clock.tick_seconds!=right->clock.tick_seconds)return Status::incompatible_contract;
    return concatenate_contracts(*left,*right,std::size_t(*axis),result);
}
}
