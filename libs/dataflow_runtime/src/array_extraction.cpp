#include "array_extraction.h"
#include "node_state.h"
#include <cmath>
#include <bit>
#include <type_traits>
#include <utility>

namespace dataflow_runtime::detail {
namespace {
template<typename T,typename Scalar> bool constant_in_range(Scalar value) noexcept {
    if constexpr(std::is_integral_v<T> && std::is_integral_v<Scalar>)return std::in_range<T>(value);
    if(!std::isfinite(value) || value<double(std::numeric_limits<T>::lowest()))return false;
    if constexpr(std::is_integral_v<T>)return std::trunc(value)==value && value<std::ldexp(1.,std::numeric_limits<T>::digits);
    return value<=double(std::numeric_limits<T>::max());
}
template<typename T,typename Scalar> bool constant_bits(Scalar value,std::uint64_t& bits) noexcept {
    if(!constant_in_range<T>(value))return false;
    using Bits=std::conditional_t<sizeof(T)==8,std::uint64_t,std::conditional_t<sizeof(T)==4,std::uint32_t,
        std::conditional_t<sizeof(T)==2,std::uint16_t,std::uint8_t>>>;
    bits=std::bit_cast<Bits>(T(value));return true;
}
template<typename Scalar> bool encode_constant(DType type,Scalar value,std::uint64_t& bits) noexcept {
    switch(type){
    case DType::Float32:return constant_bits<float>(value,bits);
    case DType::Float64:return constant_bits<double>(value,bits);
    case DType::Int8:return constant_bits<std::int8_t>(value,bits);
    case DType::UInt8:return constant_bits<std::uint8_t>(value,bits);
    case DType::Int16:return constant_bits<std::int16_t>(value,bits);
    case DType::UInt16:return constant_bits<std::uint16_t>(value,bits);
    case DType::Int32:return constant_bits<std::int32_t>(value,bits);
    case DType::UInt32:return constant_bits<std::uint32_t>(value,bits);
    case DType::Int64:return constant_bits<std::int64_t>(value,bits);
    case DType::UInt64:return constant_bits<std::uint64_t>(value,bits);
    }
    return false;
}
bool encode_padding(const NumericContract& input,const NumericLiteral& value,std::uint64_t& bits) noexcept {
    if(!std::visit([&](auto scalar){return encode_constant(input.dtype,scalar,bits);},value))return false;
    auto scalar=input;scalar.shape={};scalar.coordinates={};scalar.channel_order=input.channel_order.first(1);
    scalar.representation.alignment=1;scalar.semantic=Semantic::Numeric;scalar.history_element=Semantic::Numeric;
    std::array<std::byte,8> storage{};const PlaneLayout plane{};
    MutableNumericView probe{&scalar,storage,std::span(&plane,1),{1,1},{},{}};
    return store_bits(probe,{0,0,0},input.dtype,bits)==Status::ok;
}
Status selected_index(const AxisExtraction& selection,std::size_t actual,std::size_t at,std::size_t& index) noexcept {
    const auto raw=selection.indices.empty()?std::ptrdiff_t(selection.first+at):std::ptrdiff_t(selection.indices[at]);
    const auto resolved=raw<0?std::ptrdiff_t(actual)+raw:raw;
    if(resolved<0 || std::size_t(resolved)>=actual)return Status::invalid_argument;
    index=std::size_t(resolved);return Status::ok;
}
Status extraction_bits(const PlanNode::EndpointPlan& plan,const NumericView& input,ElementIndex index,std::uint64_t& bits) noexcept {
    const auto& selection=plan.extraction;auto& selected=selection.axis?index.column:index.row;
    const auto count=extraction_selected(selection,input.actual[selection.axis]);
    const bool inside=selected>=selection.before && selected-selection.before<count;
    if(!inside && selection.padding==ArrayPadding::Constant){bits=plan.extraction_constant;return Status::ok;}
    if(!count)return Status::invalid_argument;
    const auto at=inside?selected-selection.before:selected<selection.before?0:count-1;
    const auto status=selected_index(selection,input.actual[selection.axis],at,selected);
    return status==Status::ok?load_bits(input,index,input.contract->dtype,bits):status;
}
Status extract_row(const PlanNode::EndpointPlan& plan,const NumericView& input,MutableNumericView& output,std::size_t channel,std::size_t row) noexcept {
    for(std::size_t column=0;column<output.actual[1];++column){
        std::uint64_t bits=0;const auto loaded=extraction_bits(plan,input,{channel,row,column},bits);
        if(loaded!=Status::ok)return loaded;
        const auto stored=store_bits(output,{channel,row,column},output.contract->dtype,bits);
        if(stored!=Status::ok)return stored;
    }
    return Status::ok;
}
Status extract_channel(const PlanNode::EndpointPlan& plan,const NumericView& input,MutableNumericView& output,std::size_t channel) noexcept {
    for(std::size_t row=0;row<output.actual[0];++row){
        const auto status=extract_row(plan,input,output,channel,row);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status retain_coordinates(State& state,const NumericView& input,MutableNumericView& output,std::size_t selected) noexcept {
    const auto axis=1-selected;
    if(output.contract->coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
    const auto bytes=output.actual[axis]*sizeof(double);
    if(input.coordinates.axes[axis].size()!=bytes)return Status::incompatible_contract;
    auto& storage=state.numeric_outputs[0].coordinates[axis];
    std::copy_n(input.coordinates.axes[axis].begin(),bytes,storage.begin());
    output.coordinates.axes[axis]={storage.data(),bytes};return Status::ok;
}
}
Status prepare_axis_extraction(PlanNode& node,const ResolvedProcessingNode& resolved) {
    auto& plan=*node.endpoint_plan;
    const auto& input=std::get<NumericContract>(node.numeric_input);
    if(!axis_extraction(resolved,input,plan.extraction) || !encode_padding(input,plan.extraction.constant,plan.extraction_constant))return Status::invalid_argument;
    plan.extraction_indices.assign(plan.extraction.indices.begin(),plan.extraction.indices.end());
    plan.extraction.indices=plan.extraction_indices;plan.axis_extraction=true;return Status::ok;
}
Status execute_axis_extraction(const PlanNode& node,State& state,const NumericView& input,MutableNumericView& output,const FrameLimits& limits) noexcept {
    const auto& plan=*node.endpoint_plan;
    output.actual=input.actual;
    output.actual[plan.extraction.axis]=extraction_length(plan.extraction,input.actual[plan.extraction.axis]);
    const auto coordinates=retain_coordinates(state,input,output,plan.extraction.axis);if(coordinates!=Status::ok)return coordinates;
    if(!node.node.enabled)return fill_silence(output,limits);
    for(std::size_t channel=0;channel<output.contract->channel_order.size();++channel){
        const auto status=extract_channel(plan,input,output,channel);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
