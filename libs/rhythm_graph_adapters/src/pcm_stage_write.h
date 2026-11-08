// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
template<class T>
signal_contracts::Status write_pcm_scalar(dataflow_runtime::NumericOperationOutput& output,T value,
    const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    output.value.actual={1,1};const auto status=signal_contracts::store_element(output.value,{0,0,0},value);
    if(status==signal_contracts::Status::ok){output.origin=stamp;output.status=signal_contracts::Status::ok;}return status;
}
}
