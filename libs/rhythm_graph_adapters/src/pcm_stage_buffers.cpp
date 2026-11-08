// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_buffers.h"
#include "frame_operation_io.h"
#include <functional>

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
bool pcm_stage_buffers_disjoint(std::span<const NumericOperationInput> inputs,std::span<const NumericOperationOutput> outputs) noexcept {
    return disjoint_operation_buffers(inputs,outputs) && proof_buffers_disjoint(inputs,outputs);
}
}
