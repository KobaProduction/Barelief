// SPDX-License-Identifier: AGPL-3.0-or-later
#include "descriptor_operations.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
bool DescriptorOperation::calculate(std::span<const NumericOperationInput> inputs) noexcept {
    const auto magnitude=input_at(inputs,0)->contiguous_float32;
    const auto* phase=input_at(inputs,1);const auto angles=phase?phase->contiguous_float32:std::span<const float>{};
    switch(plan_.kind){
    case DescriptorKind::Onset:return std::get<PreparedOnsetDescriptor>(kernel_).process(magnitude,angles,result_);
    case DescriptorKind::Spectral:return std::get<PreparedSpectralOnset>(kernel_).process(magnitude,angles,result_);
    }
    return false;
}
Status DescriptorOperation::process(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    NumericOperationControl control) noexcept {
    invalidate_outputs(outputs);
    if(unsigned(control.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
    if(!control.enabled){
        if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs) || !proof_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
        return publish_disabled(outputs,operation_frame_stamp(input_at(inputs,0),false));
    }
    return process_enabled(inputs,outputs);
}
Status DescriptorOperation::process_enabled(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) noexcept {
    auto status=validate_call(inputs,outputs);if(status!=Status::ok)return status;
    if(!calculate(inputs))return Status::invalid_argument;
    status=publish(outputs,operation_point_stamp(input_at(inputs,0)));
    if(status!=Status::ok)invalidate_outputs(outputs);
    return status;
}
Status DescriptorOperation::publish_disabled(std::span<NumericOperationOutput> outputs,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    auto status=validate_outputs(outputs,limits_);if(status!=Status::ok)return status;
    for(auto& output:outputs){
        const auto shape=descriptor_output_shape(plan_,output.port);
        output.value.actual={std::size_t(shape.extents[0].maximum),std::size_t(shape.extents[1].maximum)};
        status=fill_silence(output.value,limits_);if(status!=Status::ok){invalidate_outputs(outputs);return status;}
        output.origin=stamp;output.status=Status::ok;
    }
    return Status::ok;
}
Status DescriptorOperation::publish(std::span<NumericOperationOutput> outputs,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    auto status=write_scalar(*output_at(outputs,0),result_,stamp);if(status!=Status::ok)return status;
    if(plan_.histogram){
        const auto& kernel=std::get<PreparedOnsetDescriptor>(kernel_);
        status=write_vector(*output_at(outputs,1),kernel.histogram_counts(),stamp);if(status!=Status::ok)return status;
        return write_vector(*output_at(outputs,2),kernel.rescaled_histogram_values(),stamp);
    }
    return plan_.intermediate?write_vector(*output_at(outputs,1),std::get<PreparedSpectralOnset>(kernel_).intermediate_values(),stamp):Status::ok;
}
}
