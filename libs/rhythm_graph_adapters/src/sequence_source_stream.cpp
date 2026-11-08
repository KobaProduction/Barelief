// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "sequence_source_io.h"
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
bool SequenceSourceOperation::calculate_stream(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) noexcept {
    if(plan_.kind==SequenceSourceKind::Activation)return std::get<PreparedActivationPeakSequence>(kernel_).process(inputs[0].contiguous_float32,plan_.reset_each_call);
    auto& difference=*output_at(outputs,0);auto& current=*output_at(outputs,1);
    if(plan_.kind==SequenceSourceKind::Difference){
        auto& source=std::get<PreparedSpectralDifferenceSequence>(kernel_);
        if(!source.process(sequence_source_rows(inputs[0]),inputs[0].value.actual[0],sequence_source_writer(difference),plan_.reset_each_call,sequence_source_writer(current)))return false;
        result_rows_=source.output_frames();return true;
    }
    return false;
}
}
