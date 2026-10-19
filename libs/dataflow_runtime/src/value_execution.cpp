#include "value_execution.h"
#include "frame_access.h"
#include <dsp_kernels/spectral_weights.h>
#include <algorithm>
namespace dataflow_runtime::detail {
namespace {
Status row_coordinates(State& state,const NumericView& source,MutableNumericView& output) noexcept {
    if(output.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
    const auto bytes=source.actual[1]*8;if(source.coordinates.axes[1].size()!=bytes)return Status::incompatible_contract;
    auto& destination=state.numeric_outputs[0].coordinates[0];std::copy_n(source.coordinates.axes[1].begin(),bytes,destination.begin());
    output.coordinates.axes[0]={destination.data(),bytes};return Status::ok;
}
Status copy_row(const NumericView& source,std::size_t row,MutableNumericView& output) noexcept {
    for(std::size_t column=0;column<source.actual[1];++column){
        std::uint64_t bits=0;auto status=load_bits(source,{0,row,column},source.contract->dtype,bits);if(status!=Status::ok)return status;
        status=store_bits(output,{0,column,0},output.contract->dtype,bits);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status frequency_step(const PlanNode& node,const NumericView& source,MutableNumericView& output) noexcept {
    std::uint32_t rate=0;const auto status=load_element(source,{0,0,0},rate);
    if(status!=Status::ok || !rate)return status==Status::ok?Status::unavailable:status;
    return store_element(output,{0,0,0},node.node.enabled?double(rate)*node.spectrum_reference:0.);
}
Status metadata_value(const PlanNode& node,const NumericView& source,MutableNumericView& output) noexcept {
    // SampleRate is an explicit data-to-scalar operator. Its value belongs to
    // this node's admitted input declaration; consumers need a separate edge.
    const auto value=node.node.kind==NodeKind::SampleRate?std::uint64_t(std::get<NumericContract>(node.numeric_input).clock.sample_rate):source.contract->shape.rank==Rank::Scalar?std::uint64_t{1}:std::uint64_t(source.actual[node.matrix_row]);
    if(node.node.kind==NodeKind::SampleRate && !value)return Status::unavailable;
    if(value>UINT32_MAX)return Status::overflow;
    if(node.node.kind==NodeKind::FrequencyStep)return frequency_step(node,source,output);
    return node.node.kind==NodeKind::WindowGain?store_element(output,{0,0,0},node.node.enabled?node.spectrum_reference:0.):store_element(output,{0,0,0},node.node.enabled?std::uint32_t(value):0u);
}
}
Status execute_matrix_row(const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits) noexcept {
    const auto& source=std::get<NumericView>(current.payload);auto output=mutable_output(node,state,0);
    if(node.matrix_row>=source.actual[0])return Status::unavailable;
    output.actual[0]=source.actual[1];
    const auto coordinates=row_coordinates(state,source,output);if(coordinates!=Status::ok)return coordinates;
    const auto status=node.node.enabled?copy_row(source,node.matrix_row,output):fill_silence(output,limits);
    if(status==Status::ok)state.numeric_frames[0]={current.stamp,output.view()};return status;
}
Status execute_array_fill(const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits&) noexcept {
    const auto& source=std::get<NumericView>(current.payload);const auto output=mutable_output(node,state,0);std::uint32_t count=0;
    if(load_element(source,{0,0,0},count)!=Status::ok || count!=output.actual[0])return Status::incompatible_contract;
    state.numeric_frames[0]={current.stamp,output.view()};return Status::ok;
}
Status execute_metadata(const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits&) noexcept {
    const auto& source=std::get<NumericView>(current.payload);auto output=mutable_output(node,state,0);
    const auto status=metadata_value(node,source,output);
    if(status==Status::ok)state.numeric_frames[0]={current.stamp,output.view()};return status;
}
Status execute_extract(const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits) noexcept {
    const auto& source=std::get<NumericView>(current.payload);auto output=mutable_output(node,state,0);std::uint64_t bits=0;Status status;
    if(node.node.enabled){
        const auto& index=node.endpoint_plan->indices;status=load_bits(source,{index[0],index[1],index[2]},source.contract->dtype,bits);if(status!=Status::ok)return status;
        status=store_bits(output,{0,0,0},output.contract->dtype,bits);
    }else status=fill_silence(output,limits);
    if(status==Status::ok)state.numeric_frames[0]={current.stamp,output.view()};return status;
}

Status execute_spectrum_peak(const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits&) noexcept {
    const auto& source=std::get<NumericView>(current.payload);dsp_kernels::PositiveSpectrumPeak peak;
    if(source.ring.capacity)return Status::incompatible_contract;
    if(node.node.enabled)for(std::size_t index=0;index<source.actual[0];++index){
        float value=0;const auto loaded=load_element(source,{0,index,0},value);if(loaded!=Status::ok)return loaded;
        if(!peak.include(value,index))return Status::overflow;
    }
    float hz=0;if(!peak.frequency(node.spectrum_peak.bin_hz,node.spectrum_peak.first_bin,hz))return Status::overflow;
    for(std::size_t port=0;port<2;++port){
        auto output=mutable_output(node,state,PortId(port));
        const auto stored=store_element(output,{},port==0?hz:peak.magnitude);if(stored!=Status::ok)return stored;
        state.numeric_frames[port]={current.stamp,output.view()};
    }
    return Status::ok;
}
}
