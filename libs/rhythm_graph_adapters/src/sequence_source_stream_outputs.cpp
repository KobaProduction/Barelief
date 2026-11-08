// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "sequence_source_io.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status copied_matrix(NumericOperationOutput& output,std::size_t rows,std::size_t columns,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    output.value.actual={rows,columns};output.origin=stamp;output.status=Status::ok;return Status::ok;
}
Status activation_value(NumericOperationOutput& output,const PreparedActivationPeakSequence& source,const NumericOperationOrigin& stamp) noexcept {
    if(!output.port)return write_vector(output,source.events(),stamp);
    return output.port==1?write_pcm_scalar(output,source.frame_counter(),stamp):write_pcm_scalar(output,std::uint64_t(source.buffer_frames()),stamp);
}
}
Status SequenceSourceOperation::publish_stream(NumericOperationOutput& output,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    const auto port=output.port;
    if(plan_.kind==SequenceSourceKind::Activation)return activation_value(output,std::get<PreparedActivationPeakSequence>(kernel_),stamp);
    if(port<2)return copied_matrix(output,result_rows_,plan_.features,stamp);
    if(plan_.kind==SequenceSourceKind::Difference)return write_pcm_scalar(output,std::uint64_t(result_rows_),stamp);
    return Status::invalid_argument;
}
}
