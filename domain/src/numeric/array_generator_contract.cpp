#include <domain/numeric/contracts.h>
#include <algorithm>
#include <cmath>
namespace domain {
using namespace signal_contracts;
namespace {
Status copy_generator_values(std::span<const float> values,std::span<float> destination,std::size_t& count) {
    count=values.size();
    if(values.empty())return Status::ok;
    if(!destination.empty() && destination.size()!=values.size())return Status::invalid_argument;
    if(!destination.empty())std::copy(values.begin(),values.end(),destination.begin());
    return Status::ok;
}
NumericContract generated_array_contract(const NumericContract& input,std::uint64_t count) {
    auto result=input;
    result.dtype=DType::Float32;result.semantic=Semantic::Numeric;
    result.quantity=Quantity::Unspecified;result.unit=NumericUnit::Values;
    result.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{count,count},ExtentBounds{}}};
    result.coordinates={};result.representation={};
    result.representation.alignment=alignof(float);
    return result;
}
Status generator_count(const ConfiguredProcessingNode& node,double& count) {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;
    if(resolve_processing_parameter(resolved,"size",field)!=Status::ok || !std::holds_alternative<NumericLiteral>(field))return Status::invalid_argument;
    count=std::visit([](auto v){return double(v);},std::get<NumericLiteral>(field));
    return std::isfinite(count) && count>=1 && count<=UINT32_MAX && std::trunc(count)==count?Status::ok:Status::invalid_argument;
}
Status generator_precision(const ConfiguredProcessingNode& node,NumericContract& result) {
    if(node.kind!="WindowFunction")return Status::ok;
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Transform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView field;
    if(resolve_processing_parameter(resolved,"precision",field)!=Status::ok || !std::holds_alternative<NumericLiteral>(field))return Status::invalid_argument;
    const auto precision=std::visit([](auto value){return double(value);},std::get<NumericLiteral>(field));
    if(precision!=0 && precision!=1)return Status::invalid_argument;
    result.dtype=precision?DType::Float64:DType::Float32;
    result.representation.alignment=precision?alignof(double):alignof(float);return Status::ok;
}
}
Status array_generator_values(const ConfiguredProcessingNode& node,std::span<float> destination,std::size_t& count) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ArrayFill,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView parameter;
    if(resolve_processing_parameter(resolved,"values",parameter)!=Status::ok)return Status::invalid_argument;
    const auto* values=std::get_if<std::span<const float>>(&parameter);
    if(!values || values->size()>32768)return Status::invalid_argument;
    if(!std::all_of(values->begin(),values->end(),[](float v){return std::isfinite(v);}))return Status::invalid_argument;
    return copy_generator_values(*values,destination,count);
}
Status array_generator_output_contract(const ConfiguredProcessingNode& node,const NumericContract& input,OwnedProcessingContract& out) noexcept {
    if(input.dtype!=DType::UInt32 || input.shape.rank!=Rank::Scalar || input.channel_order.size()!=1 || input.representation.encoding!=StorageEncoding::Native || input.representation.normalization!=Normalization::None)return Status::invalid_argument;
    double count=0;
    if(generator_count(node,count)!=Status::ok)return Status::invalid_argument;
    if(node.kind=="ArrayFill"){std::size_t supplied=0;if(array_generator_values(node,{},supplied)!=Status::ok || (supplied && supplied!=std::size_t(count)))return Status::invalid_argument;}
    auto result=generated_array_contract(input,std::uint64_t(count));
    if(generator_precision(node,result)!=Status::ok)return Status::invalid_argument;
    return own_processing_contract(result,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}

}
