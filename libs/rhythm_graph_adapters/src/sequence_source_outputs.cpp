// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sequence_source_operations.h"
#include "sequence_source_io.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
Status SequenceSourceOperation::publish_value(NumericOperationOutput& output,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    switch(plan_.kind){
    case SequenceSourceKind::Fusion:case SequenceSourceKind::Selection:case SequenceSourceKind::Events:return publish_onsets(output,stamp);
    case SequenceSourceKind::IntervalSummary:return publish_summary(output,std::get<PreparedBeatIntervalSummary>(kernel_),stamp);
    default:return publish_stream(output,stamp);
    }
}
}
