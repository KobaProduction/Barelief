#pragma once
#include <application/graph_lifecycle.h>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace application {

// Transport/storage-independent canonical DTO.
struct ProcessingDTO {
    // Required for Prepare and persistence; no alternate graph representation.
    std::optional<domain::NumericProcessingGraph> numeric{};
};
struct SessionIdentity {
    std::uint64_t client = 0, session = 0, epoch = 0;
    bool operator==(const SessionIdentity&) const = default;
};
struct ControlLimits {
    std::size_t max_sessions = 8;
    std::size_t max_pending_prepared = 1; // GLOBAL; values other than 1 are rejected.
    std::uint64_t prepared_ttl_ms = 30000;
    std::size_t max_graph_identity_bytes = 256;
    std::size_t max_prepared_bytes = 64 * 1024 * 1024;
    std::size_t max_field_string_bytes = 256;
    std::size_t max_replay_bytes_per_session = 512 * 1024;
    std::size_t max_replay_bytes_total = 4 * 1024 * 1024;
    std::size_t max_control_scratch_bytes = 512 * 1024;
};
struct NumericProcessingCapabilities {
    domain::NumericDeviceCapabilities device;
    NumericRuntimeLimits runtime;
};
struct ProcessingCapabilities {
    ControlLimits control;
    bool explicit_persist = false;
    std::optional<NumericProcessingCapabilities> numeric{};
};
// Compiled-ABI owning replay/staging requirement; invalid/overflowing profiles -> nullopt.
std::optional<std::size_t> control_snapshot_bytes(const ProcessingCapabilities&) noexcept;
class ControlClock {
public:
    virtual ~ControlClock() = default;
    virtual std::uint64_t now_ms() const noexcept = 0;
};
// R owns the single pending bundle, executor, intent and all transactional effects.
// Failed transactional calls retain working state; failed prepare/apply also retain pending.
class ProcessingControlRuntimePort {
public:
    virtual ~ProcessingControlRuntimePort() = default;
    virtual LifecycleResult prepare_numeric_transactional(const domain::NumericProcessingGraph&,const NumericRuntimeLimits&) {
        return {Status::unsupported_version,{}};
    }
    virtual LifecycleResult apply_transactional() = 0;
    virtual void discard_prepared() noexcept = 0;
    virtual MemoryRequirements pending_memory_requirements() const noexcept = 0;
    virtual LifecycleState state() const noexcept = 0;
    // Borrowed until successful apply or owner destruction; control calls serialized.
    virtual const domain::NumericProcessingGraph* working_numeric_graph() const noexcept { return nullptr; }
    // DTO-only owner prepared before Apply: creating/copying this handle must
    // not allocate. It makes exact replay durable after a successful Apply.
    virtual std::shared_ptr<const domain::NumericProcessingGraph> working_numeric_snapshot() const noexcept = 0;
    virtual std::optional<domain::ProcessingGraphAdmission> pending_numeric_admission() const noexcept { return {}; }
    virtual std::optional<domain::ProcessingGraphAdmission> working_numeric_admission() const noexcept { return {}; }
};
// Composition constructs GraphLifecycle and ProcessingCapabilities from the SAME
// effective device profile. This adapter delegates explicitly selected lifecycle policies.
class GraphLifecycleControlPort final : public ProcessingControlRuntimePort {
public:
    explicit GraphLifecycleControlPort(GraphLifecycle& lifecycle) noexcept : lifecycle_(lifecycle) {}
    LifecycleResult prepare_numeric_transactional(const domain::NumericProcessingGraph&,const NumericRuntimeLimits&) override;
    LifecycleResult apply_transactional() override;
    void discard_prepared() noexcept override;
    MemoryRequirements pending_memory_requirements() const noexcept override;
    LifecycleState state() const noexcept override;
    const domain::NumericProcessingGraph* working_numeric_graph() const noexcept override;
    std::shared_ptr<const domain::NumericProcessingGraph> working_numeric_snapshot() const noexcept override;
    std::optional<domain::ProcessingGraphAdmission> pending_numeric_admission() const noexcept override;
    std::optional<domain::ProcessingGraphAdmission> working_numeric_admission() const noexcept override;
private:
    GraphLifecycle& lifecycle_;
};
class ProcessingPersistencePort {
public:
    virtual ~ProcessingPersistencePort() = default;
    virtual Status persist_applied(const ProcessingDTO&, std::uint64_t graph_revision) = 0;
};

enum class ControlError {
    none, malformed, unsupported, invalid_graph, limit_exceeded, conflict,
    busy, foreign_preparation, no_prepared, preparation_expired, session_closed,
    resource_failure, persistence_failure
};
struct CommandHeader { std::uint64_t request_id = 0, expected_revision = 0; };
struct GetCapabilities {};
struct GetState {};
struct PrepareProcessing { const ProcessingDTO* processing = nullptr; };
struct ApplyProcessing { std::uint64_t token = 0; std::string_view graph_identity; };
struct ReleasePrepared { std::uint64_t token = 0; };
struct SetNumericProcessingParameters {
    domain::NodeId node = -1;
    domain::ProcessingParameters parameters;
    bool enabled = true;
};
struct StartProcessing {};
struct StopProcessing {};
struct ExplicitPersist { std::uint64_t graph_revision = 0; };
using ProcessingCommand = std::variant<GetCapabilities, GetState, PrepareProcessing,
    ApplyProcessing, ReleasePrepared, StartProcessing,
    StopProcessing, ExplicitPersist, SetNumericProcessingParameters>;
struct ControlState {
    std::uint64_t epoch = 0, revision = 0, graph_revision = 0, persisted_graph_revision = 0;
    LifecycleState lifecycle = LifecycleState::Empty;
    bool run_requested = false;
    std::uint64_t own_prepared_token = 0;
    const domain::NumericProcessingGraph* applied_numeric = nullptr;
};
struct ProcessingReply {
    std::uint64_t request_id = 0;
    bool accepted = false;
    ControlError error = ControlError::none;
    Status status = Status::ok;
    domain::ProcessingDiagnostic numeric_validation{};
    ControlState state;
    std::uint64_t prepared_token = 0, expires_at_ms = 0;
    MemoryRequirements memory;
    std::optional<domain::ProcessingGraphAdmission> numeric_resources{};
    const ProcessingCapabilities* capabilities = nullptr;
};
// One last-completed exact request/reply per admitted session; owned snapshots are
// bounded by effective graph/string limits and max_replay_bytes_per_session.
struct ControlSessionSlot {
    SessionIdentity identity;
    bool completed = false;
    CommandHeader last_header;
    ProcessingCommand last_command;
    std::optional<ProcessingDTO> request_processing;
    std::string request_graph_identity;
    ProcessingReply last_reply;
    std::shared_ptr<const domain::NumericProcessingGraph> reply_numeric_graph;
};
struct OpenControlSessionResult {
    ControlError error = ControlError::none;
    SessionIdentity identity;
    explicit operator bool() const noexcept { return error == ControlError::none; }
};

// Serialized synchronous application service. Caller slots, ports, clock and immutable
// effective capabilities outlive it. No codec/JSON, duplicate intent or prepared bundle.
// Replies' graph/capabilities pointers are borrowed, never queued without owned projection.
class ProcessingControlService {
public:
    ProcessingControlService(ProcessingControlRuntimePort&, ControlClock&,
        const ProcessingCapabilities&, std::span<ControlSessionSlot>, std::uint64_t epoch,
        ProcessingPersistencePort* persistence = nullptr) noexcept;
    ~ProcessingControlService();
    ProcessingControlService(const ProcessingControlService&) = delete;
    ProcessingControlService& operator=(const ProcessingControlService&) = delete;
    OpenControlSessionResult open_session(std::uint64_t client) noexcept;
    ControlError close_session(SessionIdentity) noexcept;
    bool expire_prepared() noexcept;
    // Serialized owner maintenance: expires pending work and observes public
    // state without consuming a CONTROL session or retaining a replay DTO.
    // Graph pointers remain borrowed; prepared-token authority is never exposed.
    ControlState observe_state() noexcept;
    ProcessingReply submit(SessionIdentity, CommandHeader, const ProcessingCommand&);
private:
    ControlSessionSlot* session(SessionIdentity) noexcept;
    void release() noexcept;
    ControlState snapshot(SessionIdentity) const noexcept;
    ProcessingReply result(SessionIdentity, CommandHeader, ControlError = ControlError::none,
                           Status = Status::ok) const;
    ProcessingReply prepare(SessionIdentity, CommandHeader, const ProcessingDTO&);
    ProcessingReply prepare_transaction(SessionIdentity,CommandHeader,const ProcessingDTO&,
                                        std::uint64_t now);
    void install_prepared(SessionIdentity,std::string_view,std::uint64_t now);
    ProcessingReply prepared_reply(SessionIdentity,CommandHeader) const;
    void commit_candidate() noexcept;
    bool command_busy(SessionIdentity,const ProcessingCommand&) const noexcept;
    ProcessingReply replay_reply(SessionIdentity,CommandHeader,const ProcessingCommand&,const ControlSessionSlot&) const;
    ProcessingReply unbounded_reply(SessionIdentity,CommandHeader,const ProcessingCommand&) const;
    std::optional<ProcessingReply> submission_preflight(SessionIdentity,CommandHeader,const ProcessingCommand&,const ControlSessionSlot&) const;
    ProcessingReply submit_staged(SessionIdentity,CommandHeader,const ProcessingCommand&,ControlSessionSlot&,std::uint64_t,SessionIdentity);
    std::optional<ProcessingReply> mutation_gate(SessionIdentity,CommandHeader,const ProcessingCommand&,std::uint64_t,SessionIdentity) const;
    ProcessingReply dispatch_mutation(SessionIdentity,CommandHeader,const ProcessingCommand&);
    ProcessingReply apply_candidate(SessionIdentity,CommandHeader,const ApplyProcessing&);
    ProcessingReply release_candidate(SessionIdentity,CommandHeader,const ReleasePrepared&);
    ProcessingReply set_parameters(SessionIdentity,CommandHeader,const SetNumericProcessingParameters&);
    ProcessingReply persist(SessionIdentity,CommandHeader,const ExplicitPersist&);
    ProcessingReply lifecycle_result(SessionIdentity, CommandHeader, const LifecycleResult&) const;
    ProcessingReply dispatch(SessionIdentity, CommandHeader, const ProcessingCommand&,
                             std::uint64_t expired_token, SessionIdentity expired_owner);
    bool bounded_command(const ProcessingCommand&) const noexcept;
    void stage_request(ControlSessionSlot&,CommandHeader,const ProcessingCommand&);
    void remember(ControlSessionSlot&,ControlSessionSlot&&,const ProcessingReply&) noexcept;
    ProcessingControlRuntimePort& runtime_;
    ControlClock& clock_;
    const ProcessingCapabilities& caps_;
    std::span<ControlSessionSlot> sessions_;
    ProcessingPersistencePort* persistence_;
    std::uint64_t epoch_, revision_ = 0, graph_revision_ = 0, persisted_revision_ = 0;
    std::uint64_t next_session_ = 1, next_token_ = 1;
    SessionIdentity owner_;
    std::uint64_t token_ = 0, base_revision_ = 0, expires_at_ = 0;
    std::array<char, 256> graph_identity_{};
    std::size_t graph_identity_size_ = 0;
    bool valid_config_ = false;
    ControlError config_error_ = ControlError::malformed;
};
} // namespace application
