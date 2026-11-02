#include <domain/numeric/contracts.h>
#include <algorithm>
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
Status matrix_autocorrelation_output_contract(const NumericContract& input,std::size_t max_lag,
                                             std::uint64_t frame_rate_fps,OwnedProcessingContract& out) noexcept {
    if(input.shape.rank!=Rank::Matrix || input.dtype!=DType::Float32 || input.channel_order.size()!=1 ||
       !input.shape.extents[1].maximum || max_lag>4095 || !frame_rate_fps || frame_rate_fps>1000000)return Status::incompatible_contract;
    auto result=input;result.semantic=Semantic::Numeric;result.history_element=Semantic::Numeric;
    result.quantity=Quantity::Unspecified;result.unit=NumericUnit::Values;
    result.representation.normalization=Normalization::None;result.shape.axes[1]=Axis::Time;
    result.shape.extents[1]={std::min(input.shape.extents[1].minimum,std::uint64_t(max_lag+1)),
                             std::min(input.shape.extents[1].maximum,std::uint64_t(max_lag+1))};
    result.coordinates[1]={CoordinateMode::Regular,NumericUnit::Seconds,{0,1},{1,frame_rate_fps}};
    return own_processing_contract(result,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
Status matrix_fft_output_contract(const NumericContract& input,std::size_t samples,OwnedProcessingContract& out,std::uint32_t sample_rate) noexcept {
    if(input.shape.rank!=Rank::Matrix || input.dtype!=DType::Float32 || input.channel_order.size()!=1 ||
       !input.shape.extents[0].maximum || !input.shape.extents[1].maximum ||
       samples<2 || samples>UINT32_MAX || (samples&(samples-1)))return Status::incompatible_contract;
    auto result=input;result.semantic=Semantic::Numeric;result.history_element=Semantic::Numeric;
    result.quantity=Quantity::SpectrumReal;result.unit=NumericUnit::RawAmplitude;result.representation.normalization=Normalization::None;
    result.shape.axes={input.shape.axes[0],Axis::Bin};
    result.shape.extents={input.shape.extents[0],ExtentBounds{samples/2+1,samples/2+1}};
    result.coordinates[1]={CoordinateMode::Regular,sample_rate?NumericUnit::Hertz:NumericUnit::Values,{0,1},
                           {sample_rate?std::int64_t(sample_rate):1,sample_rate?samples:1}};
    return own_processing_contract(result,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
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
Status matrix_fft_output_contracts(const NumericContract& input,std::size_t samples,
                                  std::array<OwnedProcessingContract,2>& out,std::uint32_t sample_rate) noexcept {
    std::array<OwnedProcessingContract,2> result;
    auto status=matrix_fft_output_contract(input,samples,result[0],sample_rate);
    if(status!=Status::ok)return status;
    auto phase=std::get<NumericContract>(result[0].view());
    phase.semantic=Semantic::Numeric;phase.quantity=Quantity::SpectrumImaginary;phase.unit=NumericUnit::RawAmplitude;
    status=own_processing_contract(phase,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,result[1]);
    if(status==Status::ok)out=std::move(result);
    return status;
}

}
