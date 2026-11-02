#pragma once
#include "numeric_extension_execution.h"
#include "node_state.h"

namespace dataflow_runtime::detail {
struct NumericInputLatch {
    NumericOperationInputRule rule;
    NumericContract contract;
    NumericBuffer buffer;
    FrameStamp stamp;
    std::vector<char> stream;
    std::size_t stream_size=0;
    bool ready=false;
    MutableNumericView mutable_view() noexcept;
    NumericOperationInput input() const noexcept;
    Status retain(const DataFrameView&,const FrameLimits&) noexcept;
    bool copy_state_from(const NumericInputLatch&) noexcept;
};
struct PreparedNumericExtension::Impl {
    std::shared_ptr<const NumericOperationFactory> factory;
    std::shared_ptr<const NumericOperationDescriptor> descriptor;
    std::unique_ptr<PreparedNumericOperation> processor;
    std::vector<NumericInputLatch> latches;
    std::vector<NumericOperationInput> inputs;
    std::vector<NumericOperationOutput> outputs;
    std::vector<NumericContract> output_contracts;
    std::vector<std::vector<char>> output_streams;
    std::vector<std::size_t> output_stream_sizes;
    std::vector<FrameStamp> output_stamps;
    std::vector<Status> output_statuses;
    bool triggered=false;
    NumericOperationControl control;
    Impl()=default;
    Impl(const Impl&);
    Status configure(const domain::ResolvedProcessingNode&,const NumericRuntimeProfile&);
    Status prepare_latches(const NumericRuntimeProfile&);
    Status prepare_outputs(const NumericRuntimeProfile&);
    bool owned_resources(domain::ProcessingResources&) const noexcept;
    bool compatible(const Impl&) const noexcept;
    bool copy_publication(const Impl&) noexcept;
    bool ready() const noexcept;
    Status retain(std::size_t,const DataFrameView&,const FrameLimits&) noexcept;
    Status process(const PlanNode&,State&,const FrameLimits&) noexcept;
    void targets(const PlanNode&,State&) noexcept;
    Status validate_outputs(const State&,const FrameLimits&) noexcept;
    void publish(State&) noexcept;
    void reset() noexcept;
    void consume() noexcept;
    void clear_borrowed() noexcept;
    void invalidate(State&,Status) noexcept;
    void rebind(State&) const noexcept;
};
bool extension_descriptor_valid(const domain::ResolvedProcessingNode&,const NumericOperationDescriptor&) noexcept;
bool extension_owned_resources(const domain::ResolvedProcessingNode&,const NumericRuntimeProfile&,domain::ProcessingResources&) noexcept;
} // namespace dataflow_runtime::detail
