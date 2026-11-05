// SPDX-License-Identifier: AGPL-3.0-or-later
#include "interval_operations.h"
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace signal_contracts;
using namespace dataflow_runtime;
std::unique_ptr<PreparedNumericOperation> IntervalOperation::clone() const {return std::make_unique<IntervalOperation>(*this);}
Status IntervalOperation::copy_state_from(const PreparedNumericOperation& source) noexcept {
    const auto* other=dynamic_cast<const IntervalOperation*>(&source);
    if(!other || kind_!=other->kind_ || maximum_!=other->maximum_ || !compatible(*other)){reset();return Status::incompatible_contract;}
    // No cross-batch analytical state; all source outputs/workspaces are
    // overwritten by the next complete input. Runtime copies its publication.
    return Status::ok;
}
std::size_t IntervalOperation::workspace_bytes() const noexcept {
    return std::visit([](const auto& kernel)->std::size_t {
        using T=std::decay_t<decltype(kernel)>;
        if constexpr(std::is_same_v<T,std::monostate>)return 0;
        else return kernel.workspace_bytes();
    },kernel_);
}
bool IntervalOperation::calculate(std::span<const float> values) noexcept {
    if(kind_==IntervalOperationKind::Histogram)return beat_interval_histogram(values,histogram_);
    return std::visit([&](auto& kernel){
        using T=std::decay_t<decltype(kernel)>;
        if constexpr(std::is_same_v<T,std::monostate>)return false;
        else return kernel.process(values);
    },kernel_);
}
Status IntervalOperation::validate_call(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    const NumericOperationInput*& input) const noexcept {
    input=input_at(inputs,0);
    if(!input || inputs.size()!=1 || !output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs))return Status::invalid_argument;
    const auto status=validate_vector(*input,maximum_,descriptor_->node.inputs.front().contract.view(),limits_);
    return status==Status::ok?validate_outputs(outputs,limits_):status;
}
Status IntervalOperation::process(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    NumericOperationControl control) noexcept {
    invalidate_outputs(outputs);const NumericOperationInput* input=nullptr;
    if(unsigned(control.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
    if(!control.enabled){
        input=input_at(inputs,0);
        if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs))return Status::invalid_argument;
        return publish_disabled(outputs,input?batch_stamp(input->stamp):FrameStamp{});
    }
    auto status=validate_call(inputs,outputs,input);if(status!=Status::ok)return status;
    if(control.event==NumericOperationEvent::Continue)return Status::would_block;
    if(!calculate(input->contiguous_float32))return Status::invalid_argument;
    status=publish(outputs,batch_stamp(input->stamp));
    if(status!=Status::ok)invalidate_outputs(outputs);
    return status;
}
std::array<std::size_t,2> IntervalOperation::disabled_shape(domain::PortId port) const noexcept {
    if(kind_==IntervalOperationKind::Histogram)return port==0?std::array<std::size_t,2>{250,1}:std::array<std::size_t,2>{2,3};
    if(kind_!=IntervalOperationKind::Fluctuation)return {0,1};
    const auto count=std::get<PreparedDetrendedFluctuation>(kernel_).scales().size();
    if(port==1)return {count,1};
    return port==2?std::array<std::size_t,2>{count-1,1}:std::array<std::size_t,2>{1,1};
}
Status IntervalOperation::publish_disabled(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) noexcept {
    auto status=validate_outputs(outputs,limits_);if(status!=Status::ok)return status;
    for(auto& output:outputs){
        output.value.actual=disabled_shape(output.port);status=fill_silence(output.value,limits_);
        if(status!=Status::ok){invalidate_outputs(outputs);return status;}
        output.stamp=stamp;output.status=Status::ok;
    }
    return Status::ok;
}
Status IntervalOperation::publish_histogram(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) noexcept {
    auto status=write_vector(*output_at(outputs,0),histogram_.histogram,stamp);if(status!=Status::ok)return status;
    auto& peaks=*output_at(outputs,1);peaks.value.actual={2,3};
    for(std::size_t row=0;row<2;++row){
        const auto& peak=histogram_.peaks[row];const std::array<float,3> values{peak.bpm,peak.weight,peak.spread};
        for(std::size_t column=0;column<3;++column){
            status=store_element(peaks.value,{0,row,column},values[column]);if(status!=Status::ok)return status;
        }
    }
    peaks.stamp=stamp;peaks.status=Status::ok;return Status::ok;
}
Status IntervalOperation::publish_fluctuation(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) noexcept {
    const auto& kernel=std::get<PreparedDetrendedFluctuation>(kernel_);
    auto status=write_scalar(*output_at(outputs,0),kernel.result().score,stamp);if(status!=Status::ok)return status;
    status=write_vector(*output_at(outputs,1),kernel.fluctuations(),stamp);if(status!=Status::ok)return status;
    status=write_vector(*output_at(outputs,2),kernel.exponents(),stamp);if(status!=Status::ok)return status;
    return write_scalar(*output_at(outputs,3),float(kernel.result().status),stamp);
}
Status IntervalOperation::publish(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) noexcept {
    if(kind_==IntervalOperationKind::Histogram)return publish_histogram(outputs,stamp);
    if(kind_==IntervalOperationKind::Harmonics)
        return write_vector(*output_at(outputs,0),std::get<PreparedHarmonicTempoCandidates>(kernel_).values(),stamp);
    if(kind_==IntervalOperationKind::Fluctuation)return publish_fluctuation(outputs,stamp);
    const auto& sections=std::get<PreparedTempoChangeSections>(kernel_);
    const auto status=write_vector(*output_at(outputs,0),sections.starts(),stamp);
    return status==Status::ok?write_vector(*output_at(outputs,1),sections.stops(),stamp):status;
}
}
