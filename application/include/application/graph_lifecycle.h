#pragma once
#include <domain/processing_contracts.h>
#include <cstddef>
#include <memory>
#include <optional>

namespace application {
using Status=signal_contracts::Status;
struct NumericRuntimeLimits {
    domain::ProcessingAdmissionLimits admission;
    domain::ProcessingResourceProfile resources;
    domain::PreparationValueProvider preparation_values;
};
bool same_numeric_processing_graph(const domain::NumericProcessingGraph&,const domain::NumericProcessingGraph&) noexcept;
struct MemoryRequirements {
    std::size_t state_bytes=0,buffer_bytes=0,scratch_bytes=0;
    bool fits(std::size_t budget) const noexcept;
};
class PreparedPlan {
public:
    virtual ~PreparedPlan()=default;
    virtual const domain::NumericProcessingGraph* numeric_definition() const noexcept=0;
    // DTO-only owner, created during Prepare; replay must not retain kernels/context.
    virtual std::shared_ptr<const domain::NumericProcessingGraph> numeric_snapshot() const noexcept=0;
    virtual MemoryRequirements memory_requirements() const noexcept=0;
    virtual std::optional<domain::ProcessingGraphAdmission> numeric_admission() const noexcept=0;
};
class RuntimeContext {public:virtual ~RuntimeContext()=default;};
struct PreparedExecution {
    std::shared_ptr<const PreparedPlan> plan;
    std::unique_ptr<RuntimeContext> context;
    explicit operator bool() const noexcept {return plan&&context;}
};
class NumericGraphPreparationPort {
public:
    virtual ~NumericGraphPreparationPort()=default;
    // Allocate here. Failure preserves the previous prepared bundle/working state.
    virtual Status prepare_numeric(const domain::NumericProcessingGraph&,const NumericRuntimeLimits&,PreparedExecution&)=0;
};
class GraphRuntimePort {
public:
    virtual ~GraphRuntimePort()=default;
    // Atomic frame-boundary commit: only success consumes the caller's bundle.
    virtual Status apply(PreparedExecution&&)=0;
    // Owner teardown only; not a user processing command.
    virtual Status shutdown()=0;
};
enum class LifecycleState {Empty,Ready,Running,Stopped};
struct LifecycleResult {
    Status status=Status::ok;
    domain::ProcessingDiagnostic numeric_validation{};
    explicit operator bool() const noexcept {return status==Status::ok&&bool(numeric_validation);}
};
// Composition serializes calls; one pending bundle and continuously active working plan.
class GraphLifecycle {
public:
    GraphLifecycle(NumericGraphPreparationPort&,GraphRuntimePort&,domain::NumericDeviceCapabilities,NumericRuntimeLimits);
    LifecycleResult prepare_numeric_transactional(const domain::NumericProcessingGraph&,const NumericRuntimeLimits&);
    LifecycleResult prepare_transactional(const domain::NumericProcessingGraph&);
    LifecycleResult apply_transactional();
    LifecycleResult connect(const domain::Edge&);
    LifecycleResult disconnect(domain::NodeId,std::uint16_t input_port=0);
    Status shutdown_transactional();
    void discard_prepared() noexcept {pending_={};}
    MemoryRequirements pending_memory_requirements() const noexcept {return pending_?pending_.plan->memory_requirements():MemoryRequirements{};}
    std::optional<domain::ProcessingGraphAdmission> pending_numeric_admission() const noexcept {return pending_?pending_.plan->numeric_admission():std::nullopt;}
    std::optional<domain::ProcessingGraphAdmission> working_numeric_admission() const noexcept {return working_?working_->numeric_admission():std::nullopt;}
    LifecycleState state() const noexcept {return state_;}
    const domain::NumericProcessingGraph* working_numeric_graph() const noexcept {return working_?working_->numeric_definition():nullptr;}
    std::shared_ptr<const domain::NumericProcessingGraph> working_numeric_snapshot() const noexcept {
        return working_?working_->numeric_snapshot():nullptr;
    }
    bool has_prepared() const noexcept {return bool(pending_);}
private:
    NumericGraphPreparationPort& preparation_;
    GraphRuntimePort& runtime_;
    domain::NumericDeviceCapabilities capabilities_;
    NumericRuntimeLimits limits_;
    PreparedExecution pending_;
    std::shared_ptr<const PreparedPlan> working_;
    LifecycleState state_=LifecycleState::Empty;
};
} // namespace application
