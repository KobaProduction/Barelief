#include <domain/numeric/contracts.h>
#include <algorithm>
#include <cmath>
namespace domain {
using namespace signal_contracts;
Status array_generator_values(const ConfiguredProcessingNode& node,std::span<float> destination,std::size_t& count) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ArrayFill,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView parameter;
    if(resolve_processing_parameter(resolved,"values",parameter)!=Status::ok)return Status::invalid_argument;
    const auto* values=std::get_if<std::span<const float>>(&parameter);
    if(!values || values->size()>32768)return Status::invalid_argument;
    if(!std::all_of(values->begin(),values->end(),[](float v){return std::isfinite(v);}))return Status::invalid_argument;
    count=values->size();
    if(values->empty())return Status::ok;
    if(!destination.empty() && destination.size()!=values->size())return Status::invalid_argument;
    if(!destination.empty())std::copy(values->begin(),values->end(),destination.begin());
    return Status::ok;
}
Status array_generator_output_contract(const ConfiguredProcessingNode& node,const NumericContract& input,OwnedProcessingContract& out) noexcept {
    if(input.dtype!=DType::UInt32 || input.shape.rank!=Rank::Scalar || input.channel_order.size()!=1 || input.representation.encoding!=StorageEncoding::Native || input.representation.normalization!=Normalization::None)return Status::invalid_argument;
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;if(resolve_processing_parameter(resolved,"size",field)!=Status::ok || !std::holds_alternative<NumericLiteral>(field))return Status::invalid_argument;
    const auto count=std::visit([](auto v){return double(v);},std::get<NumericLiteral>(field));if(!std::isfinite(count) || count<1 || count>UINT32_MAX || std::trunc(count)!=count)return Status::invalid_argument;
    if(node.kind=="ArrayFill"){std::size_t supplied=0;if(array_generator_values(node,{},supplied)!=Status::ok || (supplied && supplied!=std::size_t(count)))return Status::invalid_argument;}
    auto result=input;result.dtype=DType::Float32;result.semantic=Semantic::Numeric;result.quantity=Quantity::Unspecified;result.unit=NumericUnit::Values;result.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{std::uint64_t(count),std::uint64_t(count)},ExtentBounds{}}};result.coordinates={};result.representation={};result.representation.alignment=alignof(float);
    return own_processing_contract(result,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}

}
