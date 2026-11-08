// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/numeric_operation.h>

namespace rhythm_graph_adapters::detail {
class AdapterOperation:public dataflow_runtime::PreparedNumericOperation {
public:
    void bind(std::shared_ptr<const dataflow_runtime::NumericOperationDescriptor> descriptor,
              domain::ProcessingAdmissionLimits limits) noexcept {descriptor_=std::move(descriptor);admission_=limits;limits_=limits.frame;}
    bool owned_resources(domain::ProcessingResources&) const noexcept override;
    virtual std::size_t workspace_bytes() const noexcept=0;
    virtual std::size_t object_bytes() const noexcept=0;
protected:
    bool compatible(const AdapterOperation&) const noexcept;
    bool output_contracts_match(std::span<const dataflow_runtime::NumericOperationOutput>) const noexcept;
    std::shared_ptr<const dataflow_runtime::NumericOperationDescriptor> descriptor_;
    signal_contracts::FrameLimits limits_{};
    domain::ProcessingAdmissionLimits admission_{};
};
std::unique_ptr<AdapterOperation> prepare_interval_operation(const dataflow_runtime::NumericOperationNode&);
bool interval_operation_kind(std::string_view) noexcept;
domain::ProcessingSupport assess_interval_operation(const dataflow_runtime::NumericOperationNodeView&,const dataflow_runtime::NumericOperationProfile&) noexcept;
}
