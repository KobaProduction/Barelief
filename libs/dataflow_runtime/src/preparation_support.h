#pragma once
#include "node_state.h"
#include <dsp_kernels/scalar_reduce.h>
namespace dataflow_runtime::detail {
bool numeric_add(std::size_t&,std::size_t) noexcept;
bool numeric_mul(std::size_t,std::size_t,std::size_t&) noexcept;
const ProcessingPort* numeric_port(std::span<const ProcessingPort>,PortId) noexcept;
bool numeric_geometry(const NumericContract&,std::size_t&,std::size_t&,std::size_t&,std::array<std::size_t,2>&) noexcept;
NumericContract constant_literal_contract(const NumericContract&,DType) noexcept;
bool zero_bytes(const NumericContract&) noexcept;
bool direct_window(const NumericContract&) noexcept;
bool packet_contract(const StructuredContract&) noexcept;
Status numeric_buffer(const NumericContract&,NumericBuffer&,const std::shared_ptr<const std::vector<std::byte>>& shared={});
Status numeric_buffer(const DataContract&,NumericBuffer&,const std::shared_ptr<const std::vector<std::byte>>& shared={});
NumericGatherRequest numeric_gather_request(const ResolvedProcessingNode&,const NumericRuntimeProfile&) noexcept;
dsp_kernels::ScalarReduction numeric_reduction(NumericReduction) noexcept;
NumericContract numeric_projection(NumericContract,std::span<const std::uint32_t>,bool matrix_row=false) noexcept;
ProcessingSupport numeric_support(const void*,const ResolvedProcessingNode&,const ProcessingResourceProfile&) noexcept;
Status numeric_kernel_status(dsp_kernels::NumericStatus) noexcept;
bool numeric_compatible_node(const ConfiguredProcessingNode&,const ConfiguredProcessingNode&) noexcept;
bool numeric_parameter(const ResolvedProcessingNode&,std::string_view,double&) noexcept;
}
