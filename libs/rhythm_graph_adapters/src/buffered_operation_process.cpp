// SPDX-License-Identifier: AGPL-3.0-or-later
#include "buffered_operations.h"
#include "frame_operation_io.h"
#include "parameter_values.h"
#include <algorithm>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
std::size_t disabled_count(const domain::ResolvedProcessingNode& node,const NumericOperationInput* input) noexcept {
    NumericContract expected;if(!numeric_port(node,true,0,expected))return 0;
    const auto count=input?input->value.actual[0]:std::size_t(expected.shape.extents[0].minimum);
    return std::size_t(std::clamp<std::uint64_t>(count,expected.shape.extents[0].minimum,expected.shape.extents[0].maximum));
}
}
bool BufferedOperation::calculate(std::span<const NumericOperationInput> inputs) noexcept {
    const auto current=input_at(inputs,0)->contiguous_float32;
    if(plan_.kind==BufferedOperationKind::Flux)
        return std::get<PreparedBufferedBandFlux>(kernel_).process_pair(current,input_at(inputs,1)->contiguous_float32,result_);
    const auto output=std::span(values_).first(current.size());
    return plan_.kind==BufferedOperationKind::Maximum?std::get<PreparedBufferedMaximum>(kernel_).process(current,output):
        std::get<PreparedCausalAverage>(kernel_).process(current,output);
}
Status BufferedOperation::process(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    NumericOperationControl control) noexcept {
    invalidate_outputs(outputs);
    if(unsigned(control.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
    const auto* current=input_at(inputs,0);
    if(!control.enabled){
        if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs) || !proof_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
        return publish_disabled(outputs,operation_frame_stamp(current,false),disabled_count(definition_view(descriptor_->node),current));
    }
    auto status=validate_call(inputs,outputs);if(status!=Status::ok)return status;
    if(!calculate(inputs))return Status::invalid_argument;
    status=publish(outputs,operation_frame_stamp(current,true),current->contiguous_float32.size());
    if(status!=Status::ok)invalidate_outputs(outputs);
    return status;
}
Status BufferedOperation::publish_disabled(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp,std::size_t count) noexcept {
    auto status=validate_outputs(outputs,limits_);if(status!=Status::ok)return status;
    for(auto& output:outputs){
        output.value.actual={plan_.kind==BufferedOperationKind::Flux && output.port==0?1:count,1};
        status=fill_silence(output.value,limits_);if(status!=Status::ok){invalidate_outputs(outputs);return status;}
        output.stamp=stamp;output.status=Status::ok;
    }
    return Status::ok;
}
Status BufferedOperation::publish(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp,std::size_t count) noexcept {
    if(plan_.kind!=BufferedOperationKind::Flux)return write_vector(*output_at(outputs,0),std::span<const float>(values_).first(count),stamp);
    const auto& kernel=std::get<PreparedBufferedBandFlux>(kernel_);
    auto status=write_scalar(*output_at(outputs,0),result_,stamp);if(status!=Status::ok)return status;
    status=write_vector(*output_at(outputs,1),kernel.filtered_reference(),stamp);if(status!=Status::ok)return status;
    return write_vector(*output_at(outputs,2),kernel.positive_differences(),stamp);
}
}
