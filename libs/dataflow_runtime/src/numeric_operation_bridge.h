#pragma once
#include <dataflow_runtime/numeric_operation_binding.h>
#include <signal_contracts/data_frame.h>
#include <atomic>

namespace dataflow_runtime::detail {
struct NumericOperationClock {
    std::string domain;
    signal_contracts::Ratio tick{1,1};
    std::uint32_t sample_rate=0;
};
struct NumericOperationAuthority {mutable std::atomic<std::uint64_t> next{0};};
struct NumericOperationWriterLease {struct NumericOperationInvocation* active=nullptr;};
struct NumericOperationInvocation {
    std::span<const signal_contracts::DataFrameView> inputs;
    std::span<signal_contracts::MutableNumericView> outputs;
    std::span<const NumericOperationInput> input_buffers;
    std::span<NumericOperationOutput> output_buffers;
    std::span<const std::shared_ptr<const NumericOperationClock>> clocks;
    std::span<std::array<std::span<std::byte>,2>> coordinate_storage;
    std::span<std::array<std::size_t,2>> coordinate_counts;
    std::span<std::size_t> coordinate_groups;
    std::shared_ptr<const NumericOperationAuthority> authority;
    std::shared_ptr<NumericOperationWriterLease> lease;
    std::uint64_t generation=0;
};
struct NumericOperationBindingAccess {
    static signal_contracts::NumericBufferContract storage(const signal_contracts::NumericContract&) noexcept;
    static NumericOperationPorts ports(std::span<const domain::ProcessingPort>) noexcept;
    static NumericOperationOrigin origin(const NumericOperationInvocation&,std::size_t) noexcept;
    static void read_identity(NumericOperationOrigin&,const signal_contracts::FrameStamp&) noexcept;
    static void read_timing(NumericOperationOrigin&,const signal_contracts::FrameStamp&) noexcept;
    static void write_identity(const NumericOperationOrigin&,signal_contracts::FrameStamp&) noexcept;
    static void write_timing(const NumericOperationOrigin&,signal_contracts::FrameStamp&) noexcept;
    static NumericOperationOrigin publication(NumericOperationOrigin,NumericOperationPublication) noexcept;
    static bool same_session(const NumericOperationOrigin&,const NumericOperationOrigin&) noexcept;
    static bool same_step(const NumericOperationOrigin&,const NumericOperationOrigin&) noexcept;
    static bool same_coordinates(const NumericOperationOrigin&,const NumericOperationOrigin&) noexcept;
    static NumericOperationCoordinateWriter writer(const NumericOperationInvocation&,std::size_t) noexcept;
    static bool valid_writer(const NumericOperationCoordinateWriter&,const NumericOperationInvocation&,std::size_t) noexcept;
    static signal_contracts::Status write(NumericOperationOutput&,std::size_t,std::size_t,double) noexcept;
    static signal_contracts::Status axis(NumericOperationOutput&,std::size_t,std::size_t) noexcept;
    static signal_contracts::Status stamp(const NumericOperationOrigin&,const signal_contracts::ClockContract&,
        const std::shared_ptr<const NumericOperationAuthority>&,signal_contracts::FrameStamp&) noexcept;
};
bool numeric_operation_metadata_valid(const domain::ResolvedProcessingNode&,const NumericOperationPolicy&) noexcept;
signal_contracts::Status numeric_operation_metadata_values(const NumericOperationInvocation&,
    const NumericOperationPolicy&,domain::PortDirection) noexcept;
signal_contracts::Status numeric_operation_route_coordinates(NumericOperationInvocation&,const NumericOperationPolicy&) noexcept;
} // namespace dataflow_runtime::detail
