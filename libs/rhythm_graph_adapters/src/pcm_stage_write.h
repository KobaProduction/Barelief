// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
template<class T>
signal_contracts::Status write_pcm_scalar(dataflow_runtime::NumericOperationOutput& output,T value,
    const signal_contracts::FrameStamp& stamp) noexcept {
    output.value.actual={1,1};const auto status=signal_contracts::store_element(output.value,{0,0,0},value);
    if(status==signal_contracts::Status::ok){output.stamp=stamp;output.status=signal_contracts::Status::ok;}return status;
}
signal_contracts::Status write_pcm_complex(dataflow_runtime::NumericOperationOutput&,std::span<const float>,const signal_contracts::FrameStamp&) noexcept;
}
