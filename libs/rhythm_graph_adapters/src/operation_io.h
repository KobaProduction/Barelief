// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/numeric_operation.h>

namespace rhythm_graph_adapters::detail {
using signal_contracts::Status;
const dataflow_runtime::NumericOperationInput* input_at(std::span<const dataflow_runtime::NumericOperationInput>,domain::PortId) noexcept;
dataflow_runtime::NumericOperationOutput* output_at(std::span<dataflow_runtime::NumericOperationOutput>,domain::PortId) noexcept;
bool vector_capacity(const domain::ConfiguredProcessingNode&,domain::PortDirection,domain::PortId,std::size_t&) noexcept;
bool vector_capacity(const domain::ResolvedProcessingNode&,domain::PortDirection,domain::PortId,std::size_t&) noexcept;
bool output_shape(const domain::ConfiguredProcessingNode&,domain::PortId,signal_contracts::Rank,std::size_t,std::size_t=1,bool variable_rows=false) noexcept;
bool output_shape(const domain::ResolvedProcessingNode&,domain::PortId,signal_contracts::Rank,std::size_t,std::size_t=1,bool variable_rows=false) noexcept;
bool index_output_contract(const signal_contracts::NumericContract&) noexcept;
Status validate_vector(const dataflow_runtime::NumericOperationInput&,std::size_t maximum,
    const signal_contracts::DataContract& expected,const signal_contracts::FrameLimits&) noexcept;
Status validate_outputs(std::span<dataflow_runtime::NumericOperationOutput>,const signal_contracts::FrameLimits&) noexcept;
bool disjoint_operation_buffers(std::span<const dataflow_runtime::NumericOperationInput>,
    std::span<const dataflow_runtime::NumericOperationOutput>) noexcept;
void invalidate_outputs(std::span<dataflow_runtime::NumericOperationOutput>) noexcept;
signal_contracts::FrameStamp batch_stamp(const signal_contracts::FrameStamp&) noexcept;
Status write_vector(dataflow_runtime::NumericOperationOutput&,std::span<const float>,const signal_contracts::FrameStamp&) noexcept;
Status write_scalar(dataflow_runtime::NumericOperationOutput&,float,const signal_contracts::FrameStamp&) noexcept;
}
