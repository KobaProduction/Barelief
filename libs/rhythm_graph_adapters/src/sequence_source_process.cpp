// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "frame_operation_io.h"
#include "pcm_stage_buffers.h"

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
Status SequenceSourceOperation::process(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,NumericOperationControl control) noexcept {
    invalidate_outputs(outputs);if(unsigned(control.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
    const auto* input=input_at(inputs,0);const auto stamp=input?batch_stamp(input->stamp):FrameStamp{};
    if(!control.enabled){
        if(!output_contracts_match(outputs) || !pcm_stage_buffers_disjoint(inputs,outputs))return Status::invalid_argument;
        return publish_disabled(outputs,stamp);
    }
    if(control.event==NumericOperationEvent::EndOfStream && (!input || !input->stamp.available))return Status::ok;
    auto status=preflight(inputs,outputs);if(status!=Status::ok)return status;
    if(!calculate(inputs,outputs))return Status::invalid_argument;
    status=publish(outputs,stamp);if(status!=Status::ok)invalidate_outputs(outputs);return status;
}
Status SequenceSourceOperation::publish_disabled(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) const noexcept {
    for(auto& output:outputs){
        const auto shape=sequence_source_shape(plan_,output.port);output.value.actual={shape.variable?0:shape.rows,shape.columns};
        auto status=validate_mutable(output.value,limits_);if(status!=Status::ok)return status;
        status=fill_silence(output.value,limits_);if(status!=Status::ok){invalidate_outputs(outputs);return status;}
        output.stamp=stamp;output.status=Status::ok;
    }
    return Status::ok;
}
Status SequenceSourceOperation::publish(std::span<NumericOperationOutput> outputs,const FrameStamp& stamp) noexcept {
    for(auto& output:outputs){
        auto status=publish_value(output,stamp);if(status!=Status::ok)return status;
        if(output.status==Status::ok)status=validate_values(output.value.view(),ValueDomain::FiniteOnly,limits_);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
