#pragma once
#include <domain/graph_validation.h>
#include <signal_contracts/frame.h>
#include <cstddef>
#include <memory>

namespace application {
using Status = signal_contracts::Status;
struct RuntimeLimits {
    domain::GraphLimits graph;
    std::size_t max_memory_bytes = 64 * 1024 * 1024;
    std::size_t max_frame_samples = 4096, max_history_frames = 2000;
};
struct MemoryRequirements {
    std::size_t state_bytes = 0, buffer_bytes = 0, scratch_bytes = 0;
    bool fits(std::size_t budget) const noexcept;
};
// Implemented by the executor's immutable plan, not a second plan representation.
class PreparedPlan {
public:
    virtual ~PreparedPlan() = default;
    virtual const domain::GraphDefinition& definition() const noexcept = 0;
    virtual MemoryRequirements memory_requirements() const noexcept = 0;
};
// Executor owns all mutable state/buffers here; no state may live in PreparedPlan.
class RuntimeContext {
public:
    virtual ~RuntimeContext() = default;
};
struct PreparedExecution {
    std::shared_ptr<const PreparedPlan> plan;
    std::unique_ptr<RuntimeContext> context;
    explicit operator bool() const noexcept { return plan && context; }
};
class GraphPreparationPort {
public:
    virtual ~GraphPreparationPort() = default;
    // Allocate only here. Failure leaves result empty. The order is validated.
    virtual Status prepare(const domain::GraphDefinition&, const std::vector<domain::NodeId>& order,
                           const RuntimeLimits&, PreparedExecution& result) = 0;
};
class GraphRuntimePort {
public:
    virtual ~GraphRuntimePort() = default;
    // Atomically commit on a frame boundary; preserve compatible node state.
    // Failure retains the working plan. Success consumes the bundle.
    virtual Status apply(PreparedExecution&&, bool run_requested) = 0;
    virtual Status start() = 0;
    // Cancel queued old-generation deliveries/retries and stop owned resources.
    virtual Status stop() = 0;
};
enum class LifecycleState { Empty, Ready, Running, Stopped };
struct LifecycleResult {
    Status status = Status::ok;
    domain::ValidationResult validation;
    explicit operator bool() const noexcept { return status == Status::ok && bool(validation); }
};
// Calls must be serialized by the composition root. No GUI visibility state here.
class GraphLifecycle {
public:
    GraphLifecycle(GraphPreparationPort&, GraphRuntimePort&, domain::DeviceCapabilities,
                   RuntimeLimits limits = {});
    LifecycleResult prepare(const domain::GraphDefinition&);
    LifecycleResult apply();
    LifecycleResult connect(const domain::Edge&);
    LifecycleResult disconnect(domain::NodeId to, std::uint16_t input_port = 0);
    Status start();
    Status stop();
    LifecycleState state() const noexcept { return state_; }
    const domain::GraphDefinition* working_graph() const noexcept;
    bool has_prepared() const noexcept { return bool(pending_); }
private:
    GraphPreparationPort& preparation_;
    GraphRuntimePort& runtime_;
    domain::DeviceCapabilities capabilities_;
    RuntimeLimits limits_;
    PreparedExecution pending_;
    std::shared_ptr<const PreparedPlan> working_;
    LifecycleState state_ = LifecycleState::Empty;
    bool run_requested_ = true; // Apply auto-starts until an explicit Stop.
};
}
