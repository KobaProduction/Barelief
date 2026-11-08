#pragma once
#include "numeric_extension_execution.h"
#include "node_state.h"
#include "numeric_operation_bridge.h"

namespace dataflow_runtime::detail {
struct NumericInputLatch {
    NumericOperationInputRule rule;
    NumericContract contract;
    NumericBufferContract storage_contract;
    NumericBuffer buffer;
    FrameStamp stamp;
    std::vector<char> stream;
    std::size_t stream_size=0;
    bool ready=false;
    MutableNumericView mutable_view() noexcept;
    NumericOperationInput input() const noexcept;
    DataFrameView frame() const noexcept;
    Status retain(const DataFrameView&,const FrameLimits&) noexcept;
    Status store_frame(const DataFrameView&,const NumericView&) noexcept;
    bool copy_state_from(const NumericInputLatch&) noexcept;
};
struct PreparedNumericExtension::Impl {
    std::shared_ptr<const NumericOperationFactory> factory;
    std::shared_ptr<const NumericOperationDescriptor> descriptor;
    std::unique_ptr<PreparedNumericOperation> processor;
    std::vector<ProcessingPort> input_ports,output_ports;
    std::vector<NumericInputLatch> latches;
    std::vector<NumericOperationInput> inputs;
    std::vector<NumericOperationOutput> outputs;
    std::vector<NumericContract> output_contracts;
    std::vector<NumericBufferContract> output_buffer_contracts;
    std::vector<DataFrameView> raw_inputs;
    std::vector<MutableNumericView> raw_outputs;
    std::vector<std::array<std::span<std::byte>,2>> coordinate_storage;
    std::vector<std::array<std::size_t,2>> coordinate_counts;
    std::vector<std::size_t> coordinate_groups;
    std::vector<std::shared_ptr<const NumericOperationClock>> input_clocks;
    std::shared_ptr<const NumericOperationAuthority> authority;
    std::shared_ptr<NumericOperationWriterLease> writer_lease;
    NumericOperationInvocation invocation;
    std::vector<std::vector<char>> output_streams;
    std::vector<std::size_t> output_stream_sizes;
    std::vector<FrameStamp> output_stamps;
    std::vector<Status> output_statuses;
    bool triggered=false;
    NumericOperationControl control;
    Impl()=default;
    ~Impl();
    Impl(const Impl&);
    Status configure(const domain::ResolvedProcessingNode&,const NumericRuntimeProfile&);
    Status bind(const domain::ResolvedProcessingNode&,const NumericRuntimeProfile&);
    Status validate_output_binding(std::size_t,const State&) noexcept;
    Status validate_output_frame(std::size_t,const FrameLimits&) noexcept;
    Status prepare_latches(const NumericRuntimeProfile&);
    Status prepare_outputs(const NumericRuntimeProfile&);
    void prepare_invocation();
    void begin_invocation() noexcept;
    void bind_inputs() noexcept;
    bool owned_resources(domain::ProcessingResources&) const noexcept;
    bool compatible(const Impl&) const noexcept;
    bool copy_publication(const Impl&) noexcept;
    bool ready() const noexcept;
    Status retain(std::size_t,const DataFrameView&,const FrameLimits&) noexcept;
    Status process(const PlanNode&,State&,const FrameLimits&) noexcept;
    void targets(const PlanNode&,State&) noexcept;
    Status validate_outputs(const State&,const FrameLimits&) noexcept;
    Status validate_output_frames(const FrameLimits&) noexcept;
    void publish(State&) noexcept;
    void publish_stream(std::size_t) noexcept;
    void reset() noexcept;
    void consume() noexcept;
    void clear_borrowed() noexcept;
    void invalidate(State&,Status) noexcept;
    void rebind(State&) const noexcept;
    void rebind_output(std::size_t,State&) const noexcept;
};
bool extension_descriptor_valid(const domain::ResolvedProcessingNode&,const NumericOperationDescriptor&) noexcept;
bool extension_owned_resources(const domain::ResolvedProcessingNode&,const NumericRuntimeProfile&,domain::ProcessingResources&) noexcept;
} // namespace dataflow_runtime::detail
