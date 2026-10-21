#include <domain/numeric/contracts.h>
#include <cmath>
#include <numeric>
#include <utility>
namespace domain {
using namespace signal_contracts;
Status matrix_transpose_output_contract(const NumericContract& input,OwnedProcessingContract& out) noexcept {
    if(input.shape.rank!=Rank::Matrix || input.channel_order.size()!=1)return Status::incompatible_contract;
    auto result=input;result.semantic=Semantic::Numeric;result.history_element=Semantic::Numeric;result.quantity=Quantity::Unspecified;
    std::swap(result.shape.axes[0],result.shape.axes[1]);std::swap(result.shape.extents[0],result.shape.extents[1]);
    std::swap(result.coordinates[0],result.coordinates[1]);
    return own_processing_contract(result,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
Status matrix_window_output_contract(const NumericContract& input,OwnedProcessingContract& out) noexcept {
    if(input.shape.rank!=Rank::Matrix || input.dtype!=DType::Float32 || input.channel_order.size()!=1)return Status::incompatible_contract;
    return own_processing_contract(input,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
Status matrix_center_output_contract(const NumericContract& input,OwnedProcessingContract& out) noexcept {
    if(input.shape.rank!=Rank::Matrix || input.dtype!=DType::Float32 || input.channel_order.size()!=1)return Status::incompatible_contract;
    auto result=input;result.semantic=Semantic::Numeric;result.history_element=Semantic::Numeric;result.quantity=Quantity::Unspecified;
    result.representation.normalization=Normalization::None;
    return own_processing_contract(result,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
namespace {
Status regular_fft_coordinate(const AxisCoordinates& time,std::size_t samples,AxisCoordinates& out) {
    if(time.step.numerator<=0 || !time.step.denominator)return Status::incompatible_contract;
    auto numerator=time.step.denominator,denominator=std::uint64_t(time.step.numerator);
    auto scale=std::uint64_t(samples);
    const auto divisor=std::gcd(numerator,scale);numerator/=divisor;scale/=divisor;
    const auto reduce=std::gcd(numerator,denominator);numerator/=reduce;denominator/=reduce;
    if(numerator>INT64_MAX || denominator>UINT64_MAX/scale)return Status::overflow;
    out={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{std::int64_t(numerator),denominator*scale}};
    return Status::ok;
}
Status matrix_fft_coordinates(const NumericContract& input,std::size_t samples,NumericContract& result) {
    result.coordinates[0]=input.coordinates[0];
    result.coordinates[1]={CoordinateMode::Regular,NumericUnit::Values,{0,1},{1,1}};
    const auto& time=input.coordinates[1];
    if(input.shape.axes[1]==Axis::Time && time.unit==NumericUnit::Seconds && time.mode==CoordinateMode::ExplicitFloat64LE)
        result.coordinates[1]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{0,1}};
    else if(time.unit==NumericUnit::Seconds && time.mode==CoordinateMode::Regular)
        return regular_fft_coordinate(time,samples,result.coordinates[1]);
    return Status::ok;
}
}
Status matrix_fft_output_contract(const NumericContract& input,std::size_t samples,OwnedProcessingContract& out) noexcept {
    if(input.shape.rank!=Rank::Matrix || input.dtype!=DType::Float32 || input.channel_order.size()!=1 ||
       !input.shape.extents[0].maximum || !input.shape.extents[1].maximum ||
       samples<2 || samples>UINT32_MAX || (samples&(samples-1)))return Status::incompatible_contract;
    auto result=input;result.semantic=Semantic::SpectrumMagnitude;result.history_element=Semantic::Numeric;
    result.quantity=Quantity::Unspecified;result.representation.normalization=Normalization::None;
    result.shape.axes={input.shape.axes[0],Axis::Bin};
    result.shape.extents={input.shape.extents[0],ExtentBounds{samples/2,samples/2}};
    const auto status=matrix_fft_coordinates(input,samples,result);
    if(status!=Status::ok)return status;
    const FrameLimits limits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX};
    return own_processing_contract(result,limits,SIZE_MAX,out);
}
Status matrix_row_output_contract(const NumericContract& input,OwnedProcessingContract& out) noexcept {
    if(input.shape.rank!=Rank::Matrix || input.channel_order.size()!=1)return Status::incompatible_contract;
    auto result=input;result.shape.rank=Rank::Vector;
    result.shape.axes={input.shape.axes[1],Axis::None};result.shape.extents={input.shape.extents[1],ExtentBounds{1,1}};
    result.coordinates={input.coordinates[1],AxisCoordinates{}};
    if(input.semantic==Semantic::History){result.semantic=input.history_element;result.history_element=Semantic::Numeric;}
    const FrameLimits limits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX};
    return own_processing_contract(result,limits,SIZE_MAX,out);
}

}
