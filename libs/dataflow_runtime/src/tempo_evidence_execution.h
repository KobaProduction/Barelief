#pragma once
#include <domain/processing_contracts.h>
#include <signal_contracts/data_frame.h>
#include <memory>

namespace dataflow_runtime::detail {
struct PlanNode;struct State;
// Cold-owned numeric input join and shared portable kernel. Copies own their
// buffers; no borrowed frame is retained and no FFT/history is hidden here.
class PreparedTempoEvidenceOperation {
public:
    PreparedTempoEvidenceOperation();~PreparedTempoEvidenceOperation();
    PreparedTempoEvidenceOperation(const PreparedTempoEvidenceOperation&);
    PreparedTempoEvidenceOperation& operator=(const PreparedTempoEvidenceOperation&);
    PreparedTempoEvidenceOperation(PreparedTempoEvidenceOperation&&) noexcept;
    PreparedTempoEvidenceOperation& operator=(PreparedTempoEvidenceOperation&&) noexcept;
    bool prepare(const domain::ResolvedProcessingNode&);
    void reset() noexcept;
    signal_contracts::Status accept(const PlanNode&,State&,std::size_t lane,
        const signal_contracts::DataFrameView&,const signal_contracts::FrameLimits&) noexcept;
    static bool resources(const domain::ResolvedProcessingNode&,domain::ProcessingResources&) noexcept;
private:
    struct Impl;std::unique_ptr<Impl> value_;
};
}
