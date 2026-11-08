// SPDX-License-Identifier: AGPL-3.0-or-later
#include "spectral_operations.h"
#include "operation_io.h"
#include "frame_operation_io.h"
#include <algorithm>

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
bool SpectralOperation::apply_controls(const SpectralWarmControls& controls) noexcept {
    if(plan_.kind==SpectralOperationKind::Whitening){
        auto& kernel=std::get<PreparedSpectralWhitening>(kernel_);
        return (!controls.floor || kernel.set_floor(*controls.floor)) && (!controls.relax_time || kernel.set_relax_time(*controls.relax_time));
    }
    return true;
}
bool SpectralOperation::calculate(std::span<const NumericOperationInput> inputs) noexcept {
    const auto magnitude=input_at(inputs,0)->contiguous_float32;
    switch(plan_.kind){
    case SpectralOperationKind::Polar:return std::get<PreparedPolarNovelty>(kernel_).process(magnitude,input_at(inputs,1)->contiguous_float32,values_);
    case SpectralOperationKind::Histogram:
        return plan_.phase_input()?std::get<PreparedHistogramOnset>(kernel_).process_polar(magnitude,input_at(inputs,1)->contiguous_float32,result_):
            std::get<PreparedHistogramOnset>(kernel_).process_magnitude(magnitude,result_);
    case SpectralOperationKind::Whitening:return std::get<PreparedSpectralWhitening>(kernel_).process(magnitude,values_);
    case SpectralOperationKind::Rms:return std::get<PreparedSpectralRmsFlux>(kernel_).process(magnitude,result_);
    }
    return false;
}
Status SpectralOperation::process(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    NumericOperationControl control) noexcept {
    invalidate_outputs(outputs);
    if(unsigned(control.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
    if(!control.enabled){
        if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs))return Status::invalid_argument;
        return publish_disabled(outputs,operation_frame_stamp(input_at(inputs,0),false));
    }
    return process_enabled(inputs,outputs,control);
}
Status SpectralOperation::process_enabled(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    NumericOperationControl) noexcept {
    SpectralWarmControls controls;auto status=validate_call(inputs,outputs,controls);if(status!=Status::ok)return status;
    if(!apply_controls(controls) || !calculate(inputs))return Status::invalid_argument;
    status=publish(outputs,operation_frame_stamp(input_at(inputs,0),true));
    if(status!=Status::ok)invalidate_outputs(outputs);
    return status;
}
std::size_t SpectralOperation::output_rows(domain::PortId port) const noexcept {
    if(plan_.kind==SpectralOperationKind::Rms || (plan_.kind==SpectralOperationKind::Histogram && port==0))return 1;
    return plan_.kind==SpectralOperationKind::Histogram && port==1?plan_.histogram.histogram.bins:plan_.bins;
}
Status SpectralOperation::publish_disabled(std::span<NumericOperationOutput> outputs,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    auto status=validate_outputs(outputs,limits_);if(status!=Status::ok)return status;
    for(auto& output:outputs){
        output.value.actual={output_rows(output.port),1};status=fill_silence(output.value,limits_);
        if(status!=Status::ok){invalidate_outputs(outputs);return status;}
        output.origin=stamp;output.status=Status::ok;
    }
    return Status::ok;
}
namespace {
Status histogram_result(std::span<NumericOperationOutput> outputs,const PreparedHistogramOnset& kernel,float result,const NumericOperationOrigin& stamp) noexcept {
    auto status=write_scalar(*output_at(outputs,0),result,stamp);if(status!=Status::ok)return status;
    status=write_vector(*output_at(outputs,1),kernel.histogram_counts(),stamp);if(status!=Status::ok)return status;
    return write_vector(*output_at(outputs,2),kernel.rescaled_histogram_values(),stamp);
}
}
Status SpectralOperation::publish(std::span<NumericOperationOutput> outputs,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    if(plan_.kind==SpectralOperationKind::Rms)return write_scalar(*output_at(outputs,0),result_,stamp);
    if(plan_.kind==SpectralOperationKind::Polar)return write_vector(*output_at(outputs,0),values_,stamp);
    if(plan_.kind==SpectralOperationKind::Histogram)return histogram_result(outputs,std::get<PreparedHistogramOnset>(kernel_),result_,stamp);
    const auto status=write_vector(*output_at(outputs,0),values_,stamp);if(status!=Status::ok)return status;
    const auto peaks=std::get<PreparedSpectralWhitening>(kernel_).peaks();
    return write_vector(*output_at(outputs,1),peaks,stamp);
}
}
