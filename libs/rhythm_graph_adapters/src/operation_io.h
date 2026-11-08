// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/numeric_operation.h>

namespace rhythm_graph_adapters::detail {
using signal_contracts::Status;
bool operation_input_unique(std::span<const dataflow_runtime::NumericOperationInput>,std::size_t) noexcept;
bool operation_port_unique(dataflow_runtime::NumericOperationPorts,std::size_t) noexcept;
bool operation_ports_valid(const dataflow_runtime::NumericOperationNodeView&,const signal_contracts::FrameLimits&) noexcept;
domain::ProcessingSupport complete_operation_support(domain::ProcessingSupport,std::size_t,std::size_t extra=0) noexcept;
dataflow_runtime::NumericOperationPolicy frame_operation_policy(const dataflow_runtime::NumericOperationNodeView&,domain::PortId,
    dataflow_runtime::NumericOperationFailure);
const dataflow_runtime::NumericOperationInput* input_at(std::span<const dataflow_runtime::NumericOperationInput>,domain::PortId) noexcept;
dataflow_runtime::NumericOperationOutput* output_at(std::span<dataflow_runtime::NumericOperationOutput>,domain::PortId) noexcept;
bool vector_capacity(const dataflow_runtime::NumericOperationNode&,domain::PortDirection,domain::PortId,std::size_t&) noexcept;
bool vector_capacity(const dataflow_runtime::NumericOperationNodeView&,domain::PortDirection,domain::PortId,std::size_t&) noexcept;
bool output_shape(const dataflow_runtime::NumericOperationNode&,domain::PortId,signal_contracts::Rank,std::size_t,std::size_t=1,bool variable_rows=false) noexcept;
bool output_shape(const dataflow_runtime::NumericOperationNodeView&,domain::PortId,signal_contracts::Rank,std::size_t,std::size_t=1,bool variable_rows=false) noexcept;
Status validate_vector(const dataflow_runtime::NumericOperationInput&,std::size_t maximum,
    const signal_contracts::NumericBufferContract& expected,const signal_contracts::FrameLimits&) noexcept;
Status validate_outputs(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameLimits&) noexcept;
bool disjoint_operation_buffers(std::span<const dataflow_runtime::NumericOperationInput>,
    std::span<const dataflow_runtime::NumericOperationOutput>) noexcept;
void invalidate_outputs(std::span<dataflow_runtime::NumericOperationOutput>) noexcept;
dataflow_runtime::NumericOperationOrigin batch_stamp(const dataflow_runtime::NumericOperationOrigin&) noexcept;
Status write_vector(dataflow_runtime::NumericOperationOutput&,std::span<const float>,const dataflow_runtime::NumericOperationOrigin&) noexcept;
Status write_scalar(dataflow_runtime::NumericOperationOutput&,float,const dataflow_runtime::NumericOperationOrigin&) noexcept;
}
