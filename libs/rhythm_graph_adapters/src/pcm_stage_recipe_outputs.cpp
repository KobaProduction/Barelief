// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
Status PcmStageOperation::publish_output(std::span<const NumericOperationInput> inputs,NumericOperationOutput& output,const NumericOperationOrigin& stamp) noexcept {
    if(plan_.kind==PcmStageKind::Tracker)return publish_tracker(output,stamp);
    if(output.port==context_sequence_ports::onset_history)
        return write_vector(output,input_at(inputs,context_sequence_ports::history_input)->contiguous_float32,stamp);
    return publish_context(output,output.port,std::get<PreparedContextBeatSequence>(kernel_),stamp);
}
Status PcmStageOperation::publish(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,const NumericOperationOrigin& stamp) noexcept {
    for(auto& output:outputs){
        auto status=publish_output(inputs,output,stamp);if(status!=Status::ok)return status;
        status=validate_values(output.value.view(),pcm_stage_output_spec(plan_,output.port).values,limits_);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
