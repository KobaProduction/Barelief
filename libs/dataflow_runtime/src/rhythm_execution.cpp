#include "operation_kernel_state.h"
#include "preparation_support.h"
#include "frame_access.h"
#include <cmath>

namespace dataflow_runtime::detail {
using namespace domain;
using namespace signal_contracts;
namespace r=dsp_kernels::rhythm;
namespace {
bool read_config(const ResolvedProcessingNode& node,r::Config& config,double& sample_rate) noexcept {
    double fps=0,budget=0;
    if(!numeric_parameter(node,"feature_rate_fps",fps) || !numeric_parameter(node,"budget_ops",budget) || !numeric_parameter(node,"sample_rate",sample_rate))return false;
    if(fps<0 || fps>UINT32_MAX || budget<0 || budget>UINT32_MAX || sample_rate<=0)return false;
    config.feature_rate_fps=unsigned(fps);config.default_budget_ops=unsigned(budget);return true;
}
std::optional<r::Mode> read_mode(std::string_view value) noexcept {
    if(value=="fixed-light")return r::Mode::Light;
    if(value=="fixed-full")return r::Mode::Full;
    if(value=="gated")return r::Mode::Gated;
    return {};
}
Status read_features(std::span<std::uint8_t> destination,const NumericView& row) noexcept {
    for(std::size_t i=0;i<destination.size();++i){
        std::uint8_t value=0;const auto loaded=load_element(row,{0,i,0},value);
        if(loaded!=Status::ok)return loaded;
        destination[i]=value;
    }
    return Status::ok;
}
Status read_latest_row(OperationKernelState& kernel,const NumericView& source) noexcept {
    if(!source.actual[0] || source.actual[1]!=kernel.quantized8.size())return Status::incompatible_contract;
    NumericContract contract;NumericView row;std::array<PlaneLayout,1> planes{};
    const auto status=history_row(source,source.actual[0]-1,row,contract,planes);
    return status==Status::ok?read_features(kernel.quantized8,row):status;
}
bool source_frame(const OperationKernelState& kernel,const DataFrameView& current,r::Frame& frame) noexcept {
    const auto& clock=std::get<NumericView>(current.payload).contract->clock;
    const double observed=double(current.stamp.timestamp_ticks)*double(clock.tick_seconds.numerator)/double(clock.tick_seconds.denominator);
    const double sample=std::round(observed*kernel.rhythm_sample_rate);
    if(!std::isfinite(sample) || sample<0 || sample>=double(UINT64_MAX))return false;
    frame.epoch=current.stamp.epoch;frame.sequence=current.stamp.sequence;frame.sample_index=std::uint64_t(sample);
    frame.profile=kernel.quantized8.size();frame.sample_rate_hz=kernel.rhythm_sample_rate;
    frame.t_obs=observed;frame.t_ready=observed;frame.t_pub=observed;frame.features=kernel.quantized8;
    return true;
}
std::optional<double> normalized_phase(const r::Output& output) noexcept {
    if(!output.grid_phase_s || !output.grid_period_s)return {};
    const double phase=std::fmod(output.rhythm_grid.t_pub-*output.grid_phase_s,*output.grid_period_s)/ *output.grid_period_s;
    return phase<0?phase+1:phase;
}
std::array<std::optional<double>,rhythm_output_count> values(const r::Output& output) noexcept {
    const auto candidate=output.candidate_count?std::optional{output.candidates[0].bpm}:std::nullopt;
    return {candidate,output.bpm,output.tempo.score,double(output.tempo.status),normalized_phase(output),output.next_beat_s,
        output.bpm_per_s,output.activity,output.periodicity,double(output.cost_units),output.accent_events.score,double(output.rhythm_grid.status)};
}
Status publish(OperationKernelState& kernel,const PlanNode& node,State& state,const DataFrameView& current) noexcept {
    const auto result=values(kernel.rhythm->output());
    for(PortId port=0;port<rhythm_output_count;++port){
        auto target=mutable_output(node,state,port);auto stamp=current.stamp;
        stamp.available=stamp.available && node.node.enabled && result[port].has_value();
        const auto status=store_element(target,{0,0,0},float(result[port].value_or(0)));
        if(status!=Status::ok)return status;
        auto view=target.view();if(!stamp.available)view.storage={};
        state.numeric_frames[port]={stamp,view};
    }
    return Status::ok;
}
Status process_rhythm_frame(OperationKernelState& kernel,const DataFrameView& current,bool boundary) noexcept {
    r::Frame frame;if(!source_frame(kernel,current,frame))return Status::invalid_argument;
    if(current.stamp.discontinuity || boundary)kernel.rhythm->reset();
    return kernel.rhythm->process(frame)?Status::ok:Status::invalid_argument;
}
}
Status prepare_rhythm(OperationKernelState& kernel,const ResolvedProcessingNode& node) {
    r::Config config;const auto mode=read_mode(operation_text(node,"mode"));
    if(!mode || !read_config(node,config,kernel.rhythm_sample_rate))return Status::invalid_argument;
    kernel.rhythm.emplace();
    if(!kernel.rhythm->prepare(config,*mode))return Status::invalid_argument;
    const auto contract=std::get<NumericContract>(node.inputs[0].contract.view());
    kernel.quantized8.resize(std::size_t(contract.shape.extents[1].maximum));return Status::ok;
}
Status execute_rhythm(OperationKernelState& kernel,const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits) noexcept {
    const auto& source=std::get<NumericView>(current.payload);
    if(!node.node.enabled || !current.stamp.available){kernel.rhythm->reset();return publish(kernel,node,state,current);}
    const auto valid=validate_view(current,limits);if(valid!=Status::ok)return valid;
    const auto read=read_latest_row(kernel,source);if(read!=Status::ok)return read;
    const auto processed=process_rhythm_frame(kernel,current,state.pending_discontinuity);if(processed!=Status::ok)return processed;
    return publish(kernel,node,state,current);
}
Status execute_rhythm_features(OperationKernelState& kernel,const PlanNode& node,State& state,const DataFrameView& current) noexcept {
    r::PackingStatistics stats;
    if(node.node.enabled){
        const bool packed=kernel.input.size()==2048
            ? r::pack_peak_amplitudes(kernel.input,kernel.quantized8,stats)
            : r::pack_rfft(kernel.input,kernel.quantized8,stats);
        if(!packed)return Status::invalid_argument;
    }
    else std::fill(kernel.quantized8.begin(),kernel.quantized8.end(),0);
    auto target=mutable_output(node,state,0);
    for(std::size_t i=0;i<kernel.quantized8.size();++i){
        const auto status=store_element(target,{0,i,0},kernel.quantized8[i]);if(status!=Status::ok)return status;
    }
    state.numeric_frames[0]={current.stamp,target.view()};return Status::ok;
}
}
