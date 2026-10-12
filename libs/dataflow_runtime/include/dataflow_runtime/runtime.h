#pragma once
#include <application/graph_lifecycle.h>
#include <signal_contracts/numeric_frame_gather.h>
#include <dsp_kernels/kernel_support.h>

namespace dataflow_runtime {
using signal_contracts::Status;
struct DeliveryStamp {std::uint64_t graph_revision=0,epoch=0;};
struct NodeStats {
    domain::NodeId node=-1;
    std::uint64_t calls=0,produced=0,errors=0,self_nanoseconds=0,max_self_nanoseconds=0,deadline_misses=0;
    std::array<std::uint64_t,16> self_histogram{};
};
struct MixerInputStats {domain::PortId port=0;std::size_t depth=0,capacity=0;std::uint64_t dropped_samples=0,underrun_samples=0;};
struct NumericRuntimeProfile {
    domain::ProcessingAdmissionLimits admission;
    domain::ProcessingResourceProfile resources;
    dsp_kernels::KernelLimits kernels;
    dsp_kernels::KernelCapabilities capabilities;
    bool history_enabled=true;
    // Platform supplies a monotonic measurement clock. No OS/chrono in DSP.
    std::uint64_t (*monotonic_nanoseconds)(void*) noexcept=nullptr;
    void* measurement_clock=nullptr;
    std::size_t max_windows_per_ingress=1024,max_callback_deliveries_per_ingress=65536;
};
struct NumericPortFrameView {
    std::string_view graph_id;
    domain::PortIdentity identity;
    domain::PortDirection direction=domain::PortDirection::Output;
    std::string_view kind;
    signal_contracts::DataContract contract;
    signal_contracts::DataFrameView frame;
    Status status=Status::ok;
    DeliveryStamp delivery;
};
// Borrowed only during the callback. Async adapters acquire bounded owning rows
// before returning; full History retention is a separate snapshot operation.
enum class NumericHistoryEventKind {Row,Empty,Reset,Unavailable};
struct NumericHistoryRowView {
    NumericHistoryEventKind event=NumericHistoryEventKind::Row;
    NumericPortFrameView endpoint;
    signal_contracts::NumericContract history_contract;
    signal_contracts::NumericGatherOutput compact;
    std::uint64_t filled_count=0,capacity=0,accepted_row_ordinal=0;
};
using NumericPortFrameCallback=void (*)(void*,const NumericPortFrameView&);
using NumericHistoryCallback=void (*)(void*,const NumericHistoryRowView&);
struct NumericRuntimeCallbacks {
    NumericPortFrameCallback output=nullptr,sink_input=nullptr;
    NumericHistoryCallback history=nullptr;
    void* user=nullptr;
};
class PreparedGraph;
class RuntimeContext final:public application::RuntimeContext {
public:
    RuntimeContext();~RuntimeContext();
    RuntimeContext(const RuntimeContext&);RuntimeContext& operator=(const RuntimeContext&);
    RuntimeContext(RuntimeContext&&) noexcept;RuntimeContext& operator=(RuntimeContext&&) noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
    friend class PreparedGraph;friend class GraphExecutor;
};
class PreparedGraph final:public application::PreparedPlan {
public:
    ~PreparedGraph();
    application::MemoryRequirements memory_requirements() const noexcept override;
    const domain::NumericProcessingGraph* numeric_definition() const noexcept override;
    std::shared_ptr<const domain::NumericProcessingGraph> numeric_snapshot() const noexcept override;
    std::optional<domain::ProcessingGraphAdmission> numeric_admission() const noexcept override;
    static domain::NumericDeviceCapabilities numeric_capabilities(const NumericRuntimeProfile&) noexcept;
    static Status prepare_numeric(const domain::NumericProcessingGraph&,const NumericRuntimeProfile&,
        std::shared_ptr<const PreparedGraph>&,std::unique_ptr<RuntimeContext>&,
        const PreparedGraph* previous=nullptr,const RuntimeContext* previous_context=nullptr);
    Status process_numeric_ports(domain::NodeId,const signal_contracts::DataFrameView&,RuntimeContext&,
        DeliveryStamp,NumericRuntimeCallbacks={}) const noexcept;
    Status process_numeric_ports(domain::NodeId,domain::PortId,const signal_contracts::DataFrameView&,RuntimeContext&,DeliveryStamp,NumericRuntimeCallbacks={}) const noexcept;
    // External History requires the DATA row ordinal and an owner pinning its
    // pools/storage. Ordinal deltas declare append/batch updates, including full
    // ring laps. A changed producer/discontinuity declares snapshot replacement.
    // Borrowed History is rejected by process_numeric_ports before mutation.
    Status process_numeric_owned_ports(domain::NodeId,const signal_contracts::OwnedDataFrame&,RuntimeContext&,
        DeliveryStamp,NumericRuntimeCallbacks={},std::optional<std::uint64_t> history_ordinal={},std::shared_ptr<const void> retention_owner={},std::optional<std::uint64_t> history_filled_count={}) const noexcept;
    Status process_clock(domain::NodeId,const signal_contracts::FrameStamp&,RuntimeContext&,DeliveryStamp,NumericRuntimeCallbacks={}) const noexcept;
    Status advance_unavailable_source(domain::NodeId,const signal_contracts::FrameStamp&,RuntimeContext&,DeliveryStamp,NumericRuntimeCallbacks={},domain::PortId=0) const noexcept;
    std::optional<signal_contracts::DataContract> port_contract(domain::NodeId,domain::PortDirection,std::uint16_t) const noexcept;
    // Serialized full snapshot: local History always exposes capacity rows, with
    // typed-zero older padding. Occupancy and accepted ordinal are separate.
    // A known-clock local History is an available empty snapshot immediately
    // after preparation/reset; padding coordinates are not observed row times.
    // An explicit unavailable producer marker remains unavailable.
    // Borrowed immutable ring, no payload copy.
    Status history(domain::NodeId,std::uint16_t,const RuntimeContext&,signal_contracts::DataFrameView&,std::uint64_t* filled_count=nullptr,std::uint64_t* ordinal=nullptr) const noexcept;
    std::vector<domain::NodeId> clear_ingress(domain::NodeId,RuntimeContext&) const;
    void reset_history(domain::NodeId,RuntimeContext&) const noexcept;
    bool marker_current(domain::NodeId,const signal_contracts::FrameStamp&,const RuntimeContext&) const noexcept;
    void rebase_local_producers(RuntimeContext&,std::uint64_t previous_epoch,std::uint64_t next_epoch,std::uint64_t generation) const noexcept;
    std::vector<domain::NodeId> reset_nodes() const;
    std::optional<NodeStats> node_stats(domain::NodeId,const RuntimeContext&) const noexcept;
    std::optional<MixerInputStats> mixer_input_stats(domain::NodeId,domain::PortId,const RuntimeContext&) const noexcept;
private:
    struct Impl;PreparedGraph();std::unique_ptr<Impl> impl_;
    void refresh_state(RuntimeContext&,const PreparedGraph&,const RuntimeContext&) const noexcept;
    Status process_numeric_impl(domain::NodeId,const signal_contracts::DataFrameView&,RuntimeContext&,DeliveryStamp,
        NumericRuntimeCallbacks,const signal_contracts::OwnedDataFrame*,std::optional<std::uint64_t>,std::shared_ptr<const void>,std::optional<std::uint64_t>,domain::PortId=0) const noexcept;
    friend class GraphExecutor;
};
// One executor. All control and processing calls require serialized owner access.
class GraphExecutor final:public application::NumericGraphPreparationPort,public application::GraphRuntimePort {
public:
    explicit GraphExecutor(NumericRuntimeProfile);
    struct PreparationBase {
        std::shared_ptr<const PreparedGraph> plan;
        std::unique_ptr<RuntimeContext> context;
    private:
        const GraphExecutor* owner=nullptr;
        mutable std::weak_ptr<const PreparedGraph> prepared;
        mutable const application::RuntimeContext* prepared_context=nullptr;
        const RuntimeContext* borrowed_context=nullptr;
        friend class GraphExecutor;
    };
    // Snapshot/register require serialized owner access. Detached preparation
    // only reads its owning snapshot and may run outside the processing lock.
    PreparationBase snapshot_preparation_base() const;
    Status prepare_numeric_detached(const domain::NumericProcessingGraph&,const application::NumericRuntimeLimits&,
        const PreparationBase&,application::PreparedExecution&) const;
    Status register_prepared(const application::PreparedExecution&,const PreparationBase&) noexcept;
    Status prepare_numeric(const domain::NumericProcessingGraph&,const application::NumericRuntimeLimits&,application::PreparedExecution&) override;
    Status apply(application::PreparedExecution&&) override;
    Status shutdown() override;
    Status process_numeric_ports(domain::NodeId,const signal_contracts::DataFrameView&,DeliveryStamp,NumericRuntimeCallbacks={}) noexcept;
    Status process_numeric_ports(domain::NodeId,domain::PortId,const signal_contracts::DataFrameView&,DeliveryStamp,NumericRuntimeCallbacks={}) noexcept;
    Status process_numeric_owned_ports(domain::NodeId,const signal_contracts::OwnedDataFrame&,DeliveryStamp,NumericRuntimeCallbacks={},
        std::optional<std::uint64_t> history_ordinal={},std::shared_ptr<const void> retention_owner={},std::optional<std::uint64_t> history_filled_count={}) noexcept;
    Status process_clock(domain::NodeId,const signal_contracts::FrameStamp&,DeliveryStamp,NumericRuntimeCallbacks={}) noexcept;
    Status advance_unavailable_source(domain::NodeId,const signal_contracts::FrameStamp&,DeliveryStamp,NumericRuntimeCallbacks={},domain::PortId=0) noexcept;
    std::optional<NodeStats> node_stats(domain::NodeId) const noexcept;
    std::optional<MixerInputStats> mixer_input_stats(domain::NodeId,domain::PortId) const noexcept;
    bool running() const noexcept {return running_;}
    const PreparedGraph* plan() const noexcept {return plan_.get();}
    const RuntimeContext* context() const noexcept {return context_.get();}
    RuntimeContext* context() noexcept {return context_.get();}
private:
    NumericRuntimeProfile numeric_profile_;
    std::shared_ptr<const PreparedGraph> plan_;
    std::unique_ptr<RuntimeContext> context_;
    std::weak_ptr<const PreparedGraph> prepared_;
    const application::RuntimeContext* prepared_context_=nullptr;
    const PreparedGraph* prepared_base_plan_=nullptr;
    bool running_=false;
};
} // namespace dataflow_runtime
