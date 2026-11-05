// SPDX-License-Identifier: AGPL-3.0-or-later
#include "contrast_operations.h"
#include "frame_operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
bool ContrastOperation::calculate(const NumericOperationInput* input,NumericOperationEvent event) noexcept {
    rows_=input?input->contiguous_float32.size():0;count_=0;
    const auto values=input?input->contiguous_float32:std::span<const float>{};
    if(input)source_stamp_=operation_frame_stamp(input,true);
    if(!plan_.accumulator){
        auto& kernel=std::get<PreparedContrastPeakPicker>(kernel_);
        if(!kernel.process(values))return false;
        count_=kernel.onsets().size();return true;
    }
    auto& kernel=std::get<PreparedContrastPeakAccumulator>(kernel_);
    if(input && !kernel.consume(values))return false;
    count_=kernel.onsets().size();
    return event!=NumericOperationEvent::EndOfStream || kernel.finish(final_events_,count_);
}
Status ContrastOperation::process(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,
    NumericOperationControl control) noexcept {
    invalidate_outputs(outputs);
    if(unsigned(control.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
    if(!control.enabled){
        if(!output_contracts_match(outputs) || !disjoint_operation_buffers(inputs,outputs) || !proof_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
        return publish_disabled(outputs,operation_frame_stamp(input_at(inputs,0),false));
    }
    bool has_frame=false;auto status=validate_call(inputs,outputs,control,has_frame);if(status!=Status::ok)return status;
    if(!calculate(has_frame?input_at(inputs,0):nullptr,control.event))return Status::invalid_argument;
    status=publish(outputs,control.event);if(status!=Status::ok)invalidate_outputs(outputs);
    return status;
}
Status ContrastOperation::publish_disabled(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) noexcept {
    auto status=validate_contrast_outputs(outputs);if(status!=Status::ok)return status;
    for(auto& output:outputs){
        const bool counter=plan_.accumulator?output.port==1:output.port==3;
        output.value.actual={counter?1u:0u,1};status=fill_silence(output.value,limits_);
        if(status!=Status::ok){invalidate_outputs(outputs);return status;}
        output.stamp=output.port?stamp:batch_stamp(stamp);output.status=Status::ok;
    }
    return Status::ok;
}
}
