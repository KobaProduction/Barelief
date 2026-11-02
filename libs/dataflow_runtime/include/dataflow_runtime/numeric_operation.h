#pragma once
#include <domain/processing_contracts.h>
#include <signal_contracts/data_frame.h>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace dataflow_runtime {
struct NumericRuntimeProfile;
class NumericOperationFactory;
// Explicit operation protocol, independent of packet length and transport clock.
enum class NumericOperationMode { IndependentBatch, ContinuingStream };
enum class NumericOperationEvent { Continue, BatchComplete, EndOfStream };
enum class NumericOperationFailure { PreserveState, ResetState };
enum class NumericInputUse { Frame, HeldValue, Control };
enum class NumericInputMatch { IndependentBatch, SessionSequence, ExactInterval };
enum class NumericInputSession { Unspecified, SharedProvenance };
struct NumericOperationInputRule {
    domain::PortId port=0;
    NumericInputUse use=NumericInputUse::Frame;
    bool required=true;
    signal_contracts::ValueDomain values=signal_contracts::ValueDomain::FiniteOnly;
    bool operator==(const NumericOperationInputRule&) const=default;
};
struct NumericOperationPolicy {
    NumericOperationMode mode=NumericOperationMode::IndependentBatch;
    NumericInputMatch match=NumericInputMatch::IndependentBatch;
    NumericOperationFailure arithmetic_failure=NumericOperationFailure::ResetState;
    domain::PortId trigger=0;
    // Optional UInt8 scalar: 0=continue,1=batch complete,2=EOF,3=reset.
    // Reset delivers unavailable/discontinuous outputs even without a batch;
    // ordinary incomplete joins do not deliver a new output marker.
    // IndependentBatch declares a complete batch on every accepted trigger.
    std::optional<domain::PortId> control;
    std::vector<NumericOperationInputRule> inputs;
    // Empty means FiniteOnly for every output; otherwise one entry per port.
    // AnyIeee must be an explicit supported binding policy. Axes stay finite.
    std::vector<signal_contracts::ValueDomain> output_values;
    // Matched Frame ports explicitly share the session identity tuple
    // {producer_epoch,generation,epoch,graph_revision}. Stream IDs may differ.
    // ExactInterval additionally requires compatible declared clock domains,
    // tick durations and sample rates; transport clocks never supply DSP Fs.
    NumericInputSession session=NumericInputSession::Unspecified;
    bool operator==(const NumericOperationPolicy&) const=default;
};
// Created on the cold path. All parameter arrays/strings/contracts are owned.
// A factory may pin immutable coefficient/model providers here; their payload
// must be included once in its assessment, never borrowed from ResolvedNode.
struct NumericOperationDescriptor {
    domain::ConfiguredProcessingNode node;
    NumericOperationPolicy policy;
    std::shared_ptr<const void> immutable_resources;
    // Compared only, never dereferenced; prepared state pins the factory owner.
    const NumericOperationFactory* factory_identity=nullptr;
};
signal_contracts::Status own_numeric_operation_descriptor(const domain::ResolvedProcessingNode&,
    NumericOperationPolicy,std::shared_ptr<const NumericOperationDescriptor>&,const NumericOperationFactory* identity=nullptr);
// Descriptor DTO allocation payload, excluding allocator overhead, shared
// immutable contract snapshots, and the adapter-specific immutable_resources.
// Required is a conservative construction bound. Actual is checked again on
// cold preparation; failure must reject the new operation atomically.
bool numeric_operation_descriptor_required_bytes(const domain::ResolvedProcessingNode&,const domain::ProcessingAdmissionLimits&,std::size_t&) noexcept;
bool numeric_operation_descriptor_owned_bytes(const NumericOperationDescriptor&,const domain::ProcessingAdmissionLimits&,std::size_t&) noexcept;
bool numeric_operation_add_bytes(std::size_t&,std::size_t) noexcept;
bool numeric_operation_mul_bytes(std::size_t,std::size_t,std::size_t&) noexcept;

struct NumericOperationInput {
    domain::PortId port=0;
    signal_contracts::NumericView value;
    signal_contracts::FrameStamp stamp;
    signal_contracts::Status status=signal_contracts::Status::unavailable;
    // Optional proof-backed native Float32 object storage, mono contiguous
    // logical rows. Empty for other layouts/dtypes; never cast NumericView bytes.
    // Borrowed only during process(), like value.
    std::span<const float> contiguous_float32;
};
// Borrowed only during process(). Runtime owns all payload/coordinate storage.
// An operation sets actual extents, axis views into writable_coordinates,
// status, and its own per-output stamp. It must not replace payload ownership.
// Unavailable outputs are canonicalized to contract minima with no coordinate
// views before publication; their payload is not validated or exposed.
struct NumericOperationOutput {
    domain::PortId port=0;
    signal_contracts::MutableNumericView value;
    std::array<std::span<std::byte>,2> writable_coordinates;
    signal_contracts::FrameStamp stamp;
    signal_contracts::Status status=signal_contracts::Status::unavailable;
};
struct NumericOperationControl {
    NumericOperationEvent event=NumericOperationEvent::Continue;
    bool reset=false;
    // Disabled processing emits typed zeros and explicit coordinates/stamps
    // from settings, without advancing DSP state. The adapter owns those axes.
    bool enabled=true;
};
signal_contracts::Status numeric_operation_coordinate(NumericOperationOutput&,std::size_t axis,
    std::size_t index,double value) noexcept;
signal_contracts::Status numeric_operation_axis(NumericOperationOutput&,std::size_t axis,
    std::size_t count) noexcept;

class PreparedNumericOperation {
public:
    virtual ~PreparedNumericOperation()=default;
    // Cold only, with the same allocation-failure behavior as context copying.
    // Independent mutable plans/buffers and rebound self callback contexts.
    virtual std::unique_ptr<PreparedNumericOperation> clone() const=0;
    // No allocation. Incompatible operation/settings/geometry must fail;
    // runtime resets instead of claiming that continuity was retained.
    virtual signal_contracts::Status copy_state_from(const PreparedNumericOperation&) noexcept=0;
    virtual void reset() noexcept=0;
    // Read-only actual prepared allocation payload, including pinned descriptor
    // and provider exactly once, with the same categories as factory.assess.
    // Runtime rejects cold preparation if any category exceeds admission.
    virtual bool owned_resources(domain::ProcessingResources&) const noexcept=0;
    // Validate all finite/domain/shape constraints before mutating source state.
    // Arithmetic failure may have consumed a prefix: runtime invalidates every
    // output and applies the declared failure policy, not universal rollback.
    // No allocation, I/O, JSON, cloning or retention of these borrowed views.
    virtual signal_contracts::Status process(std::span<const NumericOperationInput>,
        std::span<NumericOperationOutput>,NumericOperationControl) noexcept=0;
};
class NumericOperationFactory {
public:
    virtual ~NumericOperationFactory()=default;
    // Cold routing/owning description; unsupported kind returns unavailable.
    // May only inspect this node's explicit settings/contracts, not topology.
    virtual signal_contracts::Status describe(const domain::ResolvedProcessingNode&,
        std::shared_ptr<const NumericOperationDescriptor>&) const=0;
    // Pure, no allocation/state mutation. Same admission/resource profile as
    // built-ins. Counts this descriptor/provider and one prepared processor,
    // including its nested plans, staging and stack; runtime charges its own
    // input latches/output storage/retained prior context separately. Matched
    // bindings must assess their explicit session/clock compatibility too.
    virtual domain::ProcessingSupport assess(const domain::ResolvedProcessingNode&,
        const NumericRuntimeProfile&) const noexcept=0;
    // Cold and atomic: output is replaced only on complete success. Prepared
    // state must pin descriptor/provider storage and own all mutable buffers.
    virtual signal_contracts::Status prepare(std::shared_ptr<const NumericOperationDescriptor>,
        const NumericRuntimeProfile&,std::unique_ptr<PreparedNumericOperation>&) const=0;
};
} // namespace dataflow_runtime
