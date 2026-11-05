// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
bool numeric_port(const domain::ResolvedProcessingNode&,bool input,domain::PortId,signal_contracts::NumericContract&) noexcept;
bool equal_frame_clock(const signal_contracts::ClockContract&,const signal_contracts::ClockContract&) noexcept;
bool equal_frame_identity(const dataflow_runtime::NumericOperationInput&,const dataflow_runtime::NumericOperationInput&) noexcept;
bool proof_buffers_disjoint(std::span<const dataflow_runtime::NumericOperationInput>,std::span<const dataflow_runtime::NumericOperationOutput>) noexcept;
signal_contracts::Status validate_frame_vector(const dataflow_runtime::NumericOperationInput&,
    const domain::ResolvedProcessingNode&,std::size_t bins,const signal_contracts::FrameLimits&,bool nonnegative) noexcept;
signal_contracts::Status validate_operation_control(const dataflow_runtime::NumericOperationInput&,
    const signal_contracts::NumericContract&,const signal_contracts::FrameLimits&) noexcept;
signal_contracts::FrameStamp operation_point_stamp(const dataflow_runtime::NumericOperationInput*) noexcept;
signal_contracts::FrameStamp operation_frame_stamp(const dataflow_runtime::NumericOperationInput*,bool enabled) noexcept;
}
