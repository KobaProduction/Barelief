#pragma once
#include <control/control_session_types.h>
#include <application/processing_control.h>
#include <bdsp_protocol/byte_stream.h>
#include <memory>
#include <string>
#include <bdsp_protocol/numeric_control_codec.h>
namespace control::numeric {
using AcquireStaging=signal_contracts::Status (*)(void*,std::size_t,std::span<std::byte>&) noexcept;
using ReleaseStaging=void (*)(void*) noexcept;
struct SessionConfig {
    bool endpoint_authorized=false, allow_mutations=true;
    std::string device_identity;
    std::uint64_t client_identity=0;
    std::uint32_t codec_mask=3, schema_version=bdsp_protocol::numeric_control::SchemaVersion;
    const application::ProcessingCapabilities* application_capabilities=nullptr;
    // Trusted immutable composition snapshot of implemented combinations. Its
    // backing metadata outlives session; admission remains the service's query.
    const bdsp_protocol::numeric_control::Capabilities* advertised_capabilities=nullptr;
    bdsp_protocol::numeric_control::Limits codec_limits=bdsp_protocol::numeric_control::desktop_limits();
    // Session transport buffers only. Cold graph owners and service replay are
    // bounded separately by the same immutable application admission profile.
    std::size_t peer_budget_bytes=2*1024*1024, storage_budget_bytes=8*1024*1024;
    // Maximum BDSP payload, including a fragment header. Negotiated independently
    // from complete CONTROL message size; small devices need no 64KiB packet arena.
    std::size_t max_packet_bytes=65535;
    // Read segmentation does not change the negotiated packet/message grammar.
    std::size_t read_chunk_bytes=4096;
    // Optional exclusive cold workspace lease. The aligned staging span is
    // disposable after decode; release runs before mapping/service callbacks.
    // Pair/context outlive this session. Acquisition must refuse reentry, never
    // alias primary arena, input/output wire or DATA storage, and return an empty
    // span on failure. A successful acquisition receives exactly one release.
    AcquireStaging acquire_staging=nullptr;
    ReleaseStaging release_staging=nullptr;
    void* staging_context=nullptr;
    // Trusted composition invariant: the sole runtime preparation adapter calls
    // reply_envelope_fits for EVERY candidate (boot/restore/local/remote and
    // parameter updates), before executor preparation, with these exact limits
    // and immutable capabilities. All working/pending graphs therefore fit BOTH
    // replies; dynamic capability replacement requires the conservative profile.
    // Without this guarantee the conservative owning-metadata bound is required.
    bool runtime_preflights_reply_envelope=false;
    std::uint64_t timeout_ms=5000;
};
using CommandCommitHandler=void (*)(const application::ProcessingReply&,void*);
using ReplyHandler=void (*)(const bdsp_protocol::numeric_control::Message&,void*);
struct SessionStorageRequirements {
    std::size_t persistent_bytes=0;
    // Additional temporary heap while validating immutable capabilities.
    // Exact codec measurement currently requires none.
    std::size_t validation_scratch_bytes=0;
    // Composition accounts this mutually exclusive shared lease separately.
    std::size_t external_staging_bytes=0;
};
// Sole numeric control boundary. Calls are serialized outside DSP/ISR. The
// shared application service owns revision, tokens, replay and the executor.
// Transport, clock, service and immutable profile outlive this controller.
// Reply views expire at callback return; handlers must not reenter this session.
class ControlSession final {
public:
    ControlSession(SessionConfig,application::ControlClock&,application::ProcessingControlService* =nullptr);
    static std::optional<SessionStorageRequirements> storage_requirements(const SessionConfig&) noexcept;
    ~ControlSession();
    ControlSession(const ControlSession&)=delete;
    ControlSession& operator=(const ControlSession&)=delete;
    signal_contracts::Status open(std::uint64_t nonce);
    signal_contracts::Status send(const bdsp_protocol::numeric_control::Command&,
        std::uint64_t request,std::uint64_t expected_revision=0);
    signal_contracts::Status poll(bdsp_protocol::ByteTransport&,std::int64_t device_pending_bytes=0);
    void reset() noexcept;
    void set_reply_handler(ReplyHandler,void* =nullptr) noexcept;
    // Server-only, unconditional after service submit and before fallible encoding.
    void set_command_commit_handler(CommandCommitHandler,void* =nullptr) noexcept;
    // HELLO/ACK envelope2 negotiates complete message and packet payload maxima
    // separately. CONTROL schema2, DATA4 and packet IDs remain canonical.
    // DATA uses the same parser/transport; auxiliary payloads are borrowed only
    // during this callback. Outbound packets fit packet_payload_capacity() (DATA performs
    // its own bounded fragmentation). Commands retain priority at adapter level.
    void set_packet_handler(bdsp_protocol::PacketHandler,void* =nullptr) noexcept;
    signal_contracts::Status send_packet(std::uint8_t,std::span<const std::uint8_t>);
    // Fence DATA28/30/31 only. A partially written packet finishes unchanged;
    // would_block means keep polling the SAME transport until its boundary.
    // Caller first retires its own DATA assembly/encoded owners. CONTROL and
    // session identity remain live; no parser or OS endpoint reset occurs.
    signal_contracts::Status discard_outbound_data() noexcept;
    signal_contracts::Status update_capabilities(const bdsp_protocol::numeric_control::Capabilities*);
    ControlSessionState state() const noexcept;
    ControlSessionError error() const noexcept;
    application::SessionIdentity identity() const noexcept;
    std::uint64_t nonce() const noexcept;
    std::size_t packet_payload_capacity() const noexcept;
    std::optional<bdsp_protocol::numeric_control::CodecId> codec() const noexcept;
    std::size_t pending_bytes() const noexcept;
    std::size_t assembly_bytes() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
