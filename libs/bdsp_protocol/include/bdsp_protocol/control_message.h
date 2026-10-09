#pragma once
#include <bdsp_protocol/control_limits.h>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace bdsp_protocol {
inline constexpr std::uint32_t control_schema_version = 1;
// Codec IDs are payload-format IDs, NEVER BDSP message registry IDs.
enum class ControlCodecId : std::uint8_t { cbor = 1, protobuf = 2 };
enum class WireCommandId : std::uint8_t {
    capabilities = 1, state = 2, prepare = 3, apply = 4, release = 5,
    set_parameters = 6, start = 7, stop = 8, persist = 9
};
enum class WireControlError : std::uint8_t {
    none = 0, malformed = 1, unsupported = 2, invalid_graph = 3, limit_exceeded = 4,
    conflict = 5, busy = 6, foreign_preparation = 7, no_prepared = 8,
    preparation_expired = 9, session_closed = 10, resource_failure = 11,
    persistence_failure = 12
};
enum class WireStatus : std::uint8_t {
    ok = 0, unavailable = 1, invalid_argument = 2, incompatible_contract = 3,
    buffer_too_small = 4, exhausted = 5, overflow = 6, would_block = 7,
    disconnected = 8, malformed = 9, unsupported_version = 10
};
enum class WireGraphError : std::uint8_t {
    none = 0, graph_limit = 1, invalid_node_id = 2, duplicate_node_id = 3,
    unknown_kind = 4, invalid_parameters = 5, unsupported_kind = 6,
    unsupported_frame_type = 7, output_limit = 8, missing_node_or_port = 9,
    incompatible_ports = 10, multiple_inputs = 11, cycle = 12, unsupported_version = 13
};
enum class WireLifecycle : std::uint8_t { empty = 0, ready = 1, running = 2, stopped = 3 };
enum class WireDirection : std::uint8_t { input = 0, output = 1 };
enum class WireFrameType : std::uint8_t {
    pcm = 0, window = 1, windowed = 2, stereo_spectrum = 3, spectrum = 4,
    mel = 5, history = 6, scalar = 7, wled_packet = 8
};
struct WireSessionIdentity { std::uint64_t client = 0, session = 0, epoch = 0; };
struct WireNoParameters {};
struct WireAudioSource { std::uint32_t source = 0; };
struct WireMerge { std::uint32_t merge = 2; };
struct WireGain { double gain = 1; };
struct WireMel { std::uint32_t bands = 50; };
struct WireHistory { std::uint32_t history_size = 400; };
struct WireWindow { std::uint32_t size = 4096; };
struct WireWindowFunction { std::uint32_t function = 0; };
struct WireTestSignal {
    std::uint32_t tones = 8, sample_rate = 48000;
    double minimum_hz = 73, maximum_hz = 13901, amplitude = .4;
    double period_seconds = 6, sweep_depth = .18;
};
struct WireNetwork {
    WireFrameType data_type = WireFrameType::mel;
    std::string_view provider_uuid, stream_id, host;
    std::uint32_t port = 8214;
};
struct WireRouting {
    WireFrameType data_type = WireFrameType::mel;
    std::uint16_t outputs = 2, selected = 0;
};
struct WireReduction { WireFrameType data_type = WireFrameType::mel; std::uint32_t operation = 0; };
// Explicit typed alternatives; no UI fields or opaque extension/JSON payload.
using WireParameters = std::variant<WireNoParameters, WireAudioSource, WireMerge,
    WireGain, WireMel, WireHistory, WireWindow, WireWindowFunction, WireTestSignal,
    WireNetwork, WireRouting, WireReduction>;
struct WireNode {
    std::int32_t id = -1;
    std::string_view kind; // existing canonical domain kind_id, never translated
    WireParameters parameters;
    bool enabled = true;
};
struct WireEdge { std::int32_t from = -1, to = -1; std::uint16_t output_port = 0, input_port = 0; };
struct WireProcessingGraph {
    std::string_view graph_identity;
    std::uint32_t processing_version = 1;
    std::span<const WireNode> nodes;
    std::span<const WireEdge> edges;
};
struct WireCommand {
    WireCommandId id = WireCommandId::state;
    std::uint64_t request_id = 0, expected_revision = 0;
    std::optional<WireProcessingGraph> processing; // prepare only
    std::uint64_t token = 0; // apply/release only
    std::string_view graph_identity; // apply only
    std::int32_t node = -1; // set_parameters only
    std::optional<WireParameters> parameters; // set_parameters only
    bool enabled = true; // set_parameters only
    std::uint64_t graph_revision = 0; // explicit persist only
};
struct WireDiagnostic {
    std::int32_t node = -1;
    std::string_view parameter_key;
    std::optional<WireDirection> direction;
    std::optional<std::uint16_t> port;
    std::optional<std::uint64_t> edge_index;
};
struct WireMemory { std::uint64_t state_bytes = 0, buffer_bytes = 0, scratch_bytes = 0; };
struct WireState {
    std::uint64_t epoch = 0, revision = 0, graph_revision = 0, persisted_graph_revision = 0;
    WireLifecycle lifecycle = WireLifecycle::empty;
    bool run_requested = false;
    std::optional<WireProcessingGraph> applied;
    std::uint64_t own_prepared_token = 0;
};
struct WireCapabilities {
    std::span<const std::string_view> node_kinds;
    std::span<const WireFrameType> receive_types, publish_types;
    std::uint32_t processing_version = 1;
    std::uint64_t max_wled_outputs = 1;
    std::uint64_t max_nodes = 128, max_edges = 256, max_memory_bytes = 64 * 1024 * 1024;
    std::uint64_t max_frame_samples = 4096, max_history_frames = 2000;
    std::uint64_t max_sessions = 8, max_pending_prepared = 1, prepared_ttl_ms = 30000;
    std::uint64_t max_graph_identity_bytes = 256, max_prepared_bytes = 64 * 1024 * 1024;
    std::uint64_t max_field_string_bytes = 256, max_replay_bytes_per_session = 512 * 1024;
    std::uint64_t max_replay_bytes_total = 4 * 1024 * 1024, max_control_scratch_bytes = 512 * 1024;
    bool explicit_persist = false;
};
struct WireReply {
    std::uint64_t request_id = 0;
    bool accepted = false;
    WireControlError error = WireControlError::none;
    WireStatus status = WireStatus::ok;
    WireGraphError validation_error = WireGraphError::none;
    std::int32_t validation_node = -1;
    std::uint64_t validation_edge_index = 0;
    WireDiagnostic validation_location, error_location;
    WireState state;
    std::uint64_t prepared_token = 0, expires_at_ms = 0;
    WireMemory memory;
    std::optional<WireCapabilities> capabilities;
};
struct WireControlMessage {
    std::uint32_t schema_version = control_schema_version;
    WireSessionIdentity identity;
    std::variant<WireCommand, WireReply> body;
};
// Borrowed views: caller/source owns all text and arrays until encode completes.
// Decode views point ONLY into supplied arena, never into the incoming bytes.
struct ControlDecodeArena {
    std::span<WireNode> nodes;
    std::span<WireEdge> edges;
    std::span<char> text;
    std::span<std::string_view> capability_kinds;
    std::span<WireFrameType> receive_types, publish_types;
};
ControlCodecResult validate_control_message(const WireControlMessage&,
    const ControlCodecLimits& = {}) noexcept;
// Prefer CBOR if both peers support it. Unknown bits confer no capability.
std::optional<ControlCodecId> select_control_codec(std::uint32_t local_mask,
    std::uint32_t remote_mask, std::uint32_t local_version,
    std::uint32_t remote_version) noexcept;
inline constexpr std::uint32_t control_cbor_mask = 1, control_protobuf_mask = 2;
} // namespace bdsp_protocol
