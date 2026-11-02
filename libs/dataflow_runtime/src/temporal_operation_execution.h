#pragma once
#include <domain/processing_contracts.h>
#include <signal_contracts/data_frame.h>
#include <memory>

namespace dataflow_runtime::detail {
struct PlanNode;struct State;
// Single bounded input sequence -> one synchronous set of vectors/matrices.
// No retained borrowed input, secondary executor or implicit streaming history.
class PreparedTemporalOperation {
public:
    PreparedTemporalOperation();~PreparedTemporalOperation();
    PreparedTemporalOperation(const PreparedTemporalOperation&);
    PreparedTemporalOperation& operator=(const PreparedTemporalOperation&);
    PreparedTemporalOperation(PreparedTemporalOperation&&) noexcept;
    PreparedTemporalOperation& operator=(PreparedTemporalOperation&&) noexcept;
    bool prepare(const domain::ResolvedProcessingNode&);
    void reset() noexcept;
    signal_contracts::Status execute(const PlanNode&,State&,const signal_contracts::DataFrameView&,
                                      const signal_contracts::FrameLimits&) noexcept;
    static bool resources(const domain::ResolvedProcessingNode&,domain::ProcessingResources&) noexcept;
private:
    struct Impl;std::unique_ptr<Impl> value_;
};
}
