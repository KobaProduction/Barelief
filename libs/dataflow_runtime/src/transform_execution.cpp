#include <dsp_kernels/spectral_weights.h>
#include "transform_execution.h"
#include "resampler_execution.h"
#include "window_execution.h"
#include "frame_access.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
namespace dataflow_runtime::detail {
namespace {
double explicit_coordinate(std::span<const std::byte> bytes,std::size_t at) noexcept {
    std::uint64_t bits=0;for(std::size_t byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[at*8+byte]))<<(byte*8);
    double value=0;std::memcpy(&value,&bits,8);return value;
}
void write_coordinate(std::span<std::byte> bytes,std::size_t at,double value) noexcept {
    std::uint64_t bits=0;std::memcpy(&bits,&value,8);for(std::size_t byte=0;byte<8;++byte)bytes[at*8+byte]=std::byte((bits>>(byte*8))&255);
}
TransformResult window_coefficients(const PlanNode& node,State& state,const DataFrameView& current,TransformDelivery delivery) noexcept {
    const auto& source=std::get<NumericView>(current.payload);const auto output=mutable_output(node,state,0);std::uint32_t size=0;
    if(load_element(source,{0,0,0},size)!=Status::ok || size!=output.actual[0])return {delivery.reject(delivery.user,Status::incompatible_contract),true};
    state.numeric_frames[0]={current.stamp,output.view()};return {Status::ok,false};
}
class SpectralExecution {
    const PlanNode& node;
    State& state;
    const DataFrameView& current;
    const FrameLimits& limits;
    const TransformDelivery delivery;
    const NumericView& source;
    MutableNumericView output;
    const std::size_t columns,count;
    Status validate_frequency_coordinates() const noexcept {
        if(source.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE || (node.node.kind!=NodeKind::BarkWeighting && node.node.kind!=NodeKind::Mel))return Status::ok;
        if(source.coordinates.axes[0].size()!=columns*sizeof(double) || state.frequencies.size()!=columns)return Status::incompatible_contract;
        for(std::size_t column=0;column<columns;++column){
            const auto hz=explicit_coordinate(source.coordinates.axes[0],column);
            const auto expected=state.frequencies[column];
            if(!std::isfinite(hz) || std::abs(hz-expected)>std::max(1e-6,expected*1e-6))return Status::incompatible_contract;
        }
        return Status::ok;
    }
    Status output_coordinates() noexcept {
        if(output.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
        auto& coordinates=state.numeric_outputs[0].coordinates[0];
        if(node.node.kind==NodeKind::StereoFFT)write_fft_coordinates(coordinates);
        else if(node.node.kind==NodeKind::Mel)write_mel_coordinates(coordinates);
        else if(!copy_source_coordinates(coordinates))return Status::incompatible_contract;
        output.coordinates.axes[0]={coordinates.data(),count*8};return Status::ok;
    }
    void write_fft_coordinates(std::span<std::byte> coordinates) const noexcept {
        for(std::size_t column=0;column<count;++column)
            write_coordinate(coordinates,column,double(column)*source.contract->clock.sample_rate/double(columns));
    }
    void write_mel_coordinates(std::span<std::byte> coordinates) const noexcept {
        const auto& kernel=std::get<dsp_kernels::MelFilterBank>(state.kernel);
        for(std::size_t column=0;column<count;++column)
            write_coordinate(coordinates,column,output.contract->coordinates[0].unit==NumericUnit::Hertz?
                kernel.center_hz(column):double(column));
    }
    bool copy_source_coordinates(std::span<std::byte> coordinates) const noexcept {
        if(source.coordinates.axes[0].size()!=count*8)return false;
        std::copy(source.coordinates.axes[0].begin(),source.coordinates.axes[0].end(),coordinates.begin());
        return true;
    }
    Status prepare_coordinates() noexcept {
        const auto status=validate_frequency_coordinates();return status==Status::ok?output_coordinates():status;
    }
    Status load_mel_plane(std::size_t plane) noexcept {
        for(std::size_t column=0;column<columns;++column){const auto status=load_element(source,{plane,column,0},state.left[column]);if(status!=Status::ok)return status;}
        return Status::ok;
    }
    bool process_mel() noexcept {
        auto& kernel=std::get<dsp_kernels::MelFilterBank>(state.kernel);
        return kernel.process(state.left,state.values);
    }
    Status store_mel_plane(std::size_t plane) noexcept {
        for(std::size_t column=0;column<count;++column){
            if(!std::isfinite(state.values[column]))return Status::overflow;
            const auto status=store_element(output,{plane,column,0},state.values[column]);if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    Status mel() noexcept {
        for(std::size_t plane=0;plane<output.planes.size();++plane){
            {const auto status=load_mel_plane(plane);if(status!=Status::ok)return status;}
            if(!process_mel())return Status::unavailable;
            const auto status=store_mel_plane(plane);if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    Status fft() noexcept {
        auto& kernel=std::get<dsp_kernels::MagnitudeSpectrum>(state.kernel);
        for(std::size_t plane=0;plane<output.planes.size();++plane){const auto status=kernel.process_plane(source,output,plane);if(status!=Status::ok)return status;}
        return Status::ok;
    }
    float bark_weight(std::size_t column) const noexcept {
        return state.coefficients[column];
    }
    Status merge(float& value,std::size_t column) const noexcept {
        float right=0;const auto status=load_element(source,{1,column,0},right);if(status!=Status::ok)return status;
        if(node.merge_mode==0)value=std::midpoint(value,right);
        else if(node.merge_mode==1)value=std::max(value,right);
        else {const float scale=std::max(std::abs(value),std::abs(right));value=scale?float(double(scale)*std::hypot(double(value)/scale,double(right)/scale)/std::sqrt(2.)):0.f;}
        return Status::ok;
    }
    Status transform_value(float& value,std::size_t column) const noexcept {
        if(node.node.kind==NodeKind::BarkWeighting)value*=bark_weight(column);
        else if(node.node.kind==NodeKind::SpectrumGain)value*=node.gain;
        else if(node.node.kind==NodeKind::SpectrumAmplitude)value*=node.gain*(node.half_last && column+1==count?.5f:1.f);
        else if(node.node.kind==NodeKind::SpectrumMerge)return merge(value,column);
        return Status::ok;
    }
    Status transform_sample(std::size_t plane,std::size_t column) noexcept {
        float value=0;{const auto status=load_element(source,{plane,column,0},value);if(status!=Status::ok)return status;}
        {const auto status=transform_value(value,column);if(status!=Status::ok)return status;}
        if(!std::isfinite(value))return Status::overflow;
        return store_element(output,{plane,column,0},node.node.enabled?value:0.f);
    }
    Status pointwise() noexcept {
        for(std::size_t plane=0;plane<output.planes.size();++plane)for(std::size_t column=0;column<count;++column){const auto status=transform_sample(plane,column);if(status!=Status::ok)return status;}
        return Status::ok;
    }
    Status render() noexcept {
        if(!node.node.enabled)return fill_silence(output,limits);
        const auto status=validate_values(source,ValueDomain::FiniteOnly,limits);if(status!=Status::ok)return status;
        if(node.node.kind==NodeKind::StereoFFT)return fft();
        if(node.node.kind==NodeKind::Mel)return mel();
        return pointwise();
    }
public:
    SpectralExecution(const PlanNode& prepared,State& context,const DataFrameView& input,const FrameLimits& bounds,TransformDelivery sink) noexcept
        :node(prepared),state(context),current(input),limits(bounds),delivery(sink),source(std::get<NumericView>(current.payload)),output(mutable_output(node,state,0)),columns(source.actual[0]),count(output.actual[0]){}
    TransformResult process() noexcept {
        {const auto status=prepare_coordinates();if(status!=Status::ok)return {delivery.reject(delivery.user,status),true};}
        const auto status=render();if(status==Status::ok)state.numeric_frames[0]={current.stamp,output.view()};return {status,false};
    }
};
}
TransformResult execute_transform(const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits,TransformDelivery delivery) noexcept {
    switch(node.node.kind){
    case NodeKind::WindowFunction:return window_coefficients(node,state,current,delivery);
    case NodeKind::PcmResampler:return execute_resampler(node,state,current,delivery);
    case NodeKind::Window:return execute_window(node,state,current,delivery);
    default:return SpectralExecution(node,state,current,limits,delivery).process();
    }
}
}
