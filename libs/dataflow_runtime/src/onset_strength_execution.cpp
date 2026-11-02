#include "operation_kernel_state.h"
#include "frame_access.h"
#include "preparation_support.h"
#include <domain/music/contracts.h>
#include <algorithm>
#include <array>

namespace dataflow_runtime::detail {
namespace {
dsp_kernels::OnsetAggregate aggregation(domain::OnsetStrengthAggregation value) noexcept {
    if(value==domain::OnsetStrengthAggregation::Median)return dsp_kernels::OnsetAggregate::Median;
    if(value==domain::OnsetStrengthAggregation::Sum)return dsp_kernels::OnsetAggregate::Sum;
    return dsp_kernels::OnsetAggregate::Mean;
}
dsp_kernels::OnsetStrengthConfig config(const domain::OnsetStrengthParameters& p) noexcept {
    return {p.lag,p.max_size,p.n_fft,p.hop_length,p.center,p.detrend,
        aggregation(p.aggregation)};
}
bool read_row(const void* context,std::size_t time,std::span<float> row) noexcept {
    const auto& source=*static_cast<const NumericView*>(context);
    for(std::size_t band=0;band<row.size();++band){
        const auto status=load_element(source,{0,time,band},row[band]);if(status!=Status::ok)return false;
    }
    return true;
}
Status copy_time_coordinates(const NumericView& source,State& state,MutableNumericView& destination) noexcept {
    if(destination.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
    const auto bytes=destination.actual[0]*sizeof(double);
    auto& owned=state.numeric_outputs[0].coordinates[0];
    if(source.coordinates.axes[0].size()!=bytes || owned.size()<bytes)return Status::incompatible_contract;
    std::copy_n(source.coordinates.axes[0].begin(),bytes,owned.begin());destination.coordinates.axes[0]={owned.data(),bytes};return Status::ok;
}
Status copy_feature_coordinates(const NumericView& source,State& state,MutableNumericView& destination) noexcept {
    if(destination.contract->coordinates[1].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
    const auto bytes=destination.actual[1]*sizeof(double);auto& owned=state.numeric_outputs[0].coordinates[1];
    if(source.coordinates.axes[1].size()!=bytes || owned.size()<bytes)return Status::incompatible_contract;
    std::copy_n(source.coordinates.axes[1].begin(),bytes,owned.begin());destination.coordinates.axes[1]={owned.data(),bytes};return Status::ok;
}
bool write_features(void* context,std::size_t time,std::span<const float> values) noexcept {
    auto& destination=*static_cast<MutableNumericView*>(context);
    for(std::size_t feature=0;feature<values.size();++feature)
        if(store_element(destination,{0,time,feature},values[feature])!=Status::ok)return false;
    return true;
}
Status write_envelope(std::span<const float> output,MutableNumericView& destination) noexcept {
    for(std::size_t time=0;time<output.size();++time){
        const auto status=store_element(destination,{0,time,0},output[time]);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status calculate(OperationKernelState& kernel,const PlanNode& node,const NumericView& source,MutableNumericView& destination,const FrameLimits& limits) noexcept {
    if(!node.node.enabled)return fill_silence(destination,limits);
    const auto valid=validate_values(source,ValueDomain::FiniteOnly,limits);if(valid!=Status::ok)return valid;
    if(kernel.onset_features)return kernel.onset_strength->process_features({&source,read_row},source.actual[0],{&destination,write_features})?Status::ok:Status::invalid_argument;
    const auto output=std::span(kernel.onset_envelope).first(destination.actual[0]);
    if(!kernel.onset_strength->process({&source,read_row},source.actual[0],output))return Status::invalid_argument;
    return write_envelope(output,destination);
}
std::array<std::size_t,2> geometry(const NumericContract& input) noexcept {
    if(input.shape.rank==Rank::Vector)return {1,std::size_t(input.shape.extents[0].maximum)};
    return {std::size_t(input.shape.extents[0].maximum),std::size_t(input.shape.extents[1].maximum)};
}
bool stream_storage(const NumericContract& input,const domain::OnsetStrengthParameters& p,std::size_t bands,std::size_t& bytes) noexcept {
    if(input.shape.rank!=Rank::Vector)return true;
    std::size_t count=0;
    return numeric_mul(bands,p.lag,count) && numeric_add(count,2*bands) &&
        numeric_mul(count,sizeof(float),bytes);
}
Status read_stream_frame(const NumericView& source,std::span<float> current) noexcept {
    for(std::size_t i=0;i<current.size();++i){
        const auto status=load_element(source,{0,i,0},current[i]);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
void prepare_stream_storage(OperationKernelState& kernel,std::size_t bands,std::size_t lag) {
    kernel.onset_lag=lag;kernel.onset_history.resize(bands*lag);
    kernel.onset_current.resize(bands);kernel.onset_envelope.resize(bands);
}
Status stream_values(OperationKernelState& kernel,const PlanNode& node,State& state,const NumericView& source,MutableNumericView& output,const FrameLimits& limits) noexcept {
    const auto written=write_envelope(kernel.onset_envelope,output);if(written!=Status::ok)return written;
    if(!node.node.enabled){
        const auto cleared=fill_silence(output,limits);if(cleared!=Status::ok)return cleared;
    }
    return copy_time_coordinates(source,state,output);
}
Status publish_onset_output(State& state,const FrameStamp& stamp,const MutableNumericView& output) noexcept {
    state.numeric_outputs[0].actual=output.actual;
    state.numeric_frames[0]={stamp,output.view()};return Status::ok;
}

Status execute_matrix(OperationKernelState& kernel,const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits) noexcept {
    const auto& source=std::get<NumericView>(current.payload);auto destination=mutable_output(node,state,0);
    destination.actual={kernel.onset_strength->output_frames(source.actual[0]),kernel.onset_features?source.actual[1]:1};
    const auto coordinates=copy_time_coordinates(source,state,destination);if(coordinates!=Status::ok)return coordinates;
    if(kernel.onset_features){const auto features=copy_feature_coordinates(source,state,destination);if(features!=Status::ok)return features;}
    const auto status=calculate(kernel,node,source,destination,limits);if(status!=Status::ok)return status;
    return publish_onset_output(state,current.stamp,destination);
}

Status retain_stream_frame(OperationKernelState& kernel,const NumericView& source) noexcept {
    auto current=std::span(kernel.onset_current);
    const auto loaded=read_stream_frame(source,current);if(loaded!=Status::ok)return loaded;
    auto previous=std::span(kernel.onset_history).subspan(kernel.onset_position*current.size(),current.size());
    auto difference=std::span(kernel.onset_envelope);
    if(kernel.onset_filled<kernel.onset_lag)std::fill(difference.begin(),difference.end(),0.f);
    else if(!kernel.onset_strength->positive_differences(current,previous,difference,true))return Status::invalid_argument;
    std::copy(current.begin(),current.end(),previous.begin());
    kernel.onset_position=(kernel.onset_position+1)%kernel.onset_lag;
    kernel.onset_filled=std::min(kernel.onset_filled+1,kernel.onset_lag);return Status::ok;
}
Status execute_stream(OperationKernelState& kernel,const PlanNode& node,State& state,const DataFrameView& frame,const FrameLimits& limits) noexcept {
    const auto& source=std::get<NumericView>(frame.payload);auto output=mutable_output(node,state,0);
    const auto valid=validate_values(source,ValueDomain::FiniteOnly,limits);if(valid!=Status::ok)return valid;
    if(frame.stamp.discontinuity){kernel.onset_position=0;kernel.onset_filled=0;}
    const auto retained=retain_stream_frame(kernel,source);if(retained!=Status::ok)return retained;
    const auto written=stream_values(kernel,node,state,source,output,limits);if(written!=Status::ok)return written;
    return publish_onset_output(state,frame.stamp,output);
}
}
bool onset_strength_resources(const domain::ResolvedProcessingNode& node,domain::ProcessingResources& resources) noexcept {
    domain::OnsetStrengthParameters p;if(!domain::onset_strength_parameters(node,p))return false;
    const auto input=std::get<NumericContract>(node.inputs[0].contract.view());
    const auto [frames,bands]=geometry(input);
    std::size_t kernel=0,output_bytes=0;
    if(!stream_storage(input,p,bands,output_bytes))return false;
    if(!dsp_kernels::PreparedOnsetStrength::storage_bytes(bands,frames,config(p),kernel) ||
       (input.shape.rank==Rank::Matrix && !numeric_mul(p.aggregation==domain::OnsetStrengthAggregation::Features?0:std::max(frames,p.lag),sizeof(float),output_bytes)))return false;
    return numeric_add(resources.plan_bytes,sizeof(OperationKernelState)) && numeric_add(resources.scratch_bytes,kernel) &&
        numeric_add(resources.payload_bytes,output_bytes);
}
Status prepare_onset_strength(OperationKernelState& kernel,const domain::ResolvedProcessingNode& node) {
    domain::OnsetStrengthParameters p;if(!domain::onset_strength_parameters(node,p))return Status::invalid_argument;
    const auto input=std::get<NumericContract>(node.inputs[0].contract.view());
    const auto [frames,bands]=geometry(input);
    kernel.onset_features=p.aggregation==domain::OnsetStrengthAggregation::Features;
    kernel.onset_strength.emplace();
    if(!kernel.onset_strength->prepare(bands,frames,config(p)))return Status::invalid_argument;
    if(input.shape.rank==Rank::Vector)prepare_stream_storage(kernel,bands,p.lag);
    if(!kernel.onset_features)kernel.onset_envelope.resize(std::max(frames,p.lag));
    return Status::ok;
}
Status execute_onset_strength(OperationKernelState& kernel,const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits) noexcept {
    if(std::get<NumericView>(current.payload).contract->shape.rank==Rank::Vector)return execute_stream(kernel,node,state,current,limits);
    return execute_matrix(kernel,node,state,current,limits);
}
}
