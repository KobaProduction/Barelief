#pragma once
#include <application/graph_lifecycle.h>
#include <span>

namespace dataflow_runtime {
using signal_contracts::FrameView;
using signal_contracts::PortContract;
using signal_contracts::Status;
struct IngressSpec {
    domain::NodeId node = -1;
    PortContract contract;
    // PCM block capacity is independent of the FFT window and may vary per tick.
    std::size_t max_samples = 0;
    bool allow_legacy_window = false; // explicit desktop-owned compatibility ingress only
};
struct OutputView {
    domain::NodeId node;
    domain::NodeKind kind;
    FrameView frame;
    bool window_emitted = false;
    std::span<const float> display_values; // FFTView selected/merged channel, not a second frame payload
};
// The view is borrowed for the callback only, including each history row. A
// consumer retaining output must copy/encode it outside the runtime operation.
using OutputCallback = void (*)(void *, const OutputView &);
class PreparedGraph;
class RuntimeContext final : public application::RuntimeContext {
  public:
    RuntimeContext();
    ~RuntimeContext();
    RuntimeContext(const RuntimeContext &);
    RuntimeContext &operator=(const RuntimeContext &);
    RuntimeContext(RuntimeContext &&) noexcept;
    RuntimeContext &operator=(RuntimeContext &&) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class PreparedGraph;
};
class PreparedGraph final : public application::PreparedPlan {
  public:
    ~PreparedGraph();
    const domain::GraphDefinition &definition() const noexcept override;
    application::MemoryRequirements memory_requirements() const noexcept override;
    // Preparation validates once and allocates every workspace. Previous state
    // is read only; incompatible descendants are reset, compatible rings copied
    // during this transaction, never during process().
    static Status prepare(const domain::GraphDefinition &, std::span<const IngressSpec>,
                          const application::RuntimeLimits &, std::shared_ptr<const PreparedGraph> &,
                          std::unique_ptr<RuntimeContext> &, const PreparedGraph *previous = nullptr,
                          const RuntimeContext *previous_context = nullptr);
    Status process(domain::NodeId, const FrameView &, RuntimeContext &, OutputCallback = nullptr,
                   void *user = nullptr) const noexcept;
    std::vector<domain::NodeId> clear_ingress(domain::NodeId, RuntimeContext &) const;
    bool marker_current(domain::NodeId, const FrameView &, const RuntimeContext &) const noexcept;
    signal_contracts::HistoryView history(domain::NodeId, const RuntimeContext &) const noexcept;
    // -1 means local owning ring (or no history). Otherwise the caller must
    // retain that external ingress in its own bounded owner to snapshot later.
    domain::NodeId history_ingress(domain::NodeId) const noexcept;
    void reset_history(domain::NodeId, RuntimeContext &) const noexcept;
    void rebase_local_producers(RuntimeContext &, std::uint64_t previous_epoch, std::uint64_t next_epoch,
                                std::uint64_t generation) const noexcept;
    std::vector<domain::NodeId> reset_nodes() const;

  private:
    struct Impl;
    PreparedGraph();
    std::unique_ptr<Impl> impl_;
    void refresh_state(RuntimeContext &, const PreparedGraph &, const RuntimeContext &) const noexcept;
    friend class GraphExecutor;
};
// Application lifecycle ports backed by this same executor. It owns no sockets
// or ingress queue; adapters fence/cancel their deliveries when Stop is called.
// All methods, including preparation, require serialized owner access.
class GraphExecutor final : public application::GraphPreparationPort, public application::GraphRuntimePort {
  public:
    explicit GraphExecutor(std::span<const IngressSpec> specs = {});
    void ingress_specs(std::span<const IngressSpec>); // control plane only
    Status prepare(const domain::GraphDefinition &, const std::vector<domain::NodeId> &order,
                   const application::RuntimeLimits &, application::PreparedExecution &) override;
    Status apply(application::PreparedExecution &&, bool run_requested) override;
    Status start() override;
    Status stop() override;
    Status process(domain::NodeId, const FrameView &, OutputCallback = nullptr, void *user = nullptr) noexcept;
    bool running() const noexcept {
        return running_;
    }
    const PreparedGraph *plan() const noexcept {
        return plan_.get();
    }
    const RuntimeContext *context() const noexcept {
        return context_.get();
    }
    // Serialized control-plane access for ingress invalidation/history snapshots.
    RuntimeContext *context() noexcept { return context_.get(); }

  private:
    std::vector<IngressSpec> specs_;
    std::shared_ptr<const PreparedGraph> plan_;
    std::unique_ptr<RuntimeContext> context_;
    std::weak_ptr<const PreparedGraph> prepared_;
    const application::RuntimeContext *prepared_context_ = nullptr;
    const PreparedGraph *prepared_base_plan_ = nullptr;
    bool running_ = false;
};
} // namespace dataflow_runtime
