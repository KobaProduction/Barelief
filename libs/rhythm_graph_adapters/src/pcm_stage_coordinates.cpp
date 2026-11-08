// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
Status PcmStageOperation::prepare_output_views(std::span<NumericOperationOutput> outputs) const noexcept {
    for(auto& output:outputs){
        const auto spec=pcm_stage_output_spec(plan_,output.port);
        output.value.actual={spec.variable?0:spec.rows,spec.columns};
        const auto status=validate_mutable(output.value,limits_);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
