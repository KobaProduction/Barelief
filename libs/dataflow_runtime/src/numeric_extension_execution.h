#pragma once
#include <dataflow_runtime/numeric_operation.h>

namespace dataflow_runtime::detail {
struct PlanNode;struct State;
// Generic bounded input join and output ownership; source math stays in the
// optional factory adapter. All views are rebuilt after cold context copying.
class PreparedNumericExtension {
public:
    PreparedNumericExtension();~PreparedNumericExtension();
    PreparedNumericExtension(const PreparedNumericExtension&);
    PreparedNumericExtension& operator=(const PreparedNumericExtension&);
    PreparedNumericExtension(PreparedNumericExtension&&) noexcept;
    PreparedNumericExtension& operator=(PreparedNumericExtension&&) noexcept;
    signal_contracts::Status prepare(const domain::ResolvedProcessingNode&,const NumericRuntimeProfile&);
    void reset() noexcept;
    signal_contracts::Status copy_state_from(const PreparedNumericExtension&) noexcept;
    signal_contracts::Status accept(const PlanNode&,State&,std::size_t lane,
        const signal_contracts::DataFrameView&,const signal_contracts::FrameLimits&) noexcept;
    void rebind_outputs(State&) const noexcept;
    signal_contracts::Status output_status(domain::PortId) const noexcept;
    static domain::ProcessingSupport assess(const domain::ResolvedProcessingNode&,const NumericRuntimeProfile&) noexcept;
private:
    friend bool extension_owned_resources(const domain::ResolvedProcessingNode&,const NumericRuntimeProfile&,domain::ProcessingResources&) noexcept;
    struct Impl;std::unique_ptr<Impl> value_;
};
} // namespace dataflow_runtime::detail
