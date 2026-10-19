#pragma once
#include "node_state.h"
namespace dataflow_runtime::detail {
class IngressAcceptance {
    std::span<const PlanNode> nodes;
    std::span<State> states;
    std::size_t source_index;
    PortId ingress_port;
    DataFrameView& input;
    const FrameLimits& limits;
    const OwnedDataFrame* owned;
    std::optional<std::uint64_t> history_ordinal;
    std::shared_ptr<const void>& retention_owner;
    std::optional<std::uint64_t> history_filled_count;
    const PlanNode& source_node;
    State& source_state;
    std::optional<DataContract> actual;
    Status validate_input() noexcept;
    Status declare_history() noexcept;
    Status validate_payload() const noexcept;
    Status validate_filled_progress(const NumericView&,std::uint64_t) const noexcept;
    Status validate_growing_progress(const NumericView&,std::uint64_t) const noexcept;
    bool belongs_to_lane(std::size_t) const noexcept;
    void reset_descendant(std::size_t) noexcept;
    void retire_epoch(std::uint64_t) noexcept;
    void detect_packet_gap(const FrameStamp&) noexcept;
    Status validate_history_progress(const FrameStamp&) const noexcept;
    Status validate_marker(const FrameStamp&) const noexcept;
    void reset_descendants(const FrameStamp&) noexcept;
    Status retain_history() noexcept;
public:
    const NumericContract* history_input=nullptr;
    bool external_history=false;
    std::uint64_t real_filled=0;
    IngressAcceptance(std::span<const PlanNode>,std::span<State>,std::size_t,PortId,DataFrameView&,const FrameLimits&,const OwnedDataFrame*,std::optional<std::uint64_t>,std::shared_ptr<const void>&,std::optional<std::uint64_t>) noexcept;
    Status accept() noexcept;
};
}
