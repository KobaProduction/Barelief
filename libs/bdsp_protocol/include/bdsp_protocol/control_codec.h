#pragma once
#include <bdsp_protocol/control_message.h>

namespace bdsp_protocol {
// Complete payload only: framing/fragmentation/registry negotiation are adapters.
// All caller buffers and source views must be disjoint and stable during a call.
// Error: consumed/produced==0; destination and every arena byte unchanged.
// required_* are exact on buffer_too_small after complete successful preflight;
// malformed/unsupported/limit errors publish no capacity promise.
// Encode stages in caller scratch then commits; required_scratch==required_wire.
// Decode validates/counts without writes, checks arena, then commits owned views.
class ControlCodec {
public:
    virtual ~ControlCodec() = default;
    virtual ControlCodecId id() const noexcept = 0;
    virtual ControlCodecResult encode(const WireControlMessage&, std::span<std::uint8_t> destination,
        std::span<std::uint8_t> scratch, const ControlCodecLimits& = {}) const noexcept = 0;
    virtual ControlCodecResult decode(std::span<const std::uint8_t> source, ControlDecodeArena,
        WireControlMessage& destination, const ControlCodecLimits& = {}) const noexcept = 0;
};
class CborControlCodec final : public ControlCodec {
public:
    ControlCodecId id() const noexcept override { return ControlCodecId::cbor; }
    ControlCodecResult encode(const WireControlMessage&, std::span<std::uint8_t>,
        std::span<std::uint8_t>, const ControlCodecLimits& = {}) const noexcept override;
    ControlCodecResult decode(std::span<const std::uint8_t>, ControlDecodeArena,
        WireControlMessage&, const ControlCodecLimits& = {}) const noexcept override;
};
class ProtobufControlCodec final : public ControlCodec {
public:
    ControlCodecId id() const noexcept override { return ControlCodecId::protobuf; }
    ControlCodecResult encode(const WireControlMessage&, std::span<std::uint8_t>,
        std::span<std::uint8_t>, const ControlCodecLimits& = {}) const noexcept override;
    ControlCodecResult decode(std::span<const std::uint8_t>, ControlDecodeArena,
        WireControlMessage&, const ControlCodecLimits& = {}) const noexcept override;
};

namespace detail {
// Shared schema traversal, independent of the serialization libraries. These
// implementation details are not application ports or a second control service.
enum class ControlSchema { SessionIdentity, NoParameters, AudioSource, Merge, Gain,
    Mel, History, Window, WindowFunction, TestSignal, Network, Routing, Reduction,
    Parameters, Node, Edge, ProcessingGraph, Command, Diagnostic, Memory, State,
    Capabilities, Reply, ControlMessage, TextElement, TypeElement };
class ControlArchive;
using ControlVisit = bool (*)(ControlArchive&, void*) noexcept;
using ControlElementVisit = bool (*)(ControlArchive&, const void*, std::size_t) noexcept;
struct ControlArchiveState {
    const ControlCodecLimits& limits;
    ControlDecodeArena arena;
    ControlStorageRequirements used;
    ControlCodecStatus error = ControlCodecStatus::ok;
    bool reading = false, materializing = false;
    bool fail(ControlCodecStatus status = ControlCodecStatus::malformed) noexcept;
    bool store_text(std::string_view&, std::size_t maximum) noexcept;
    bool count(std::size_t&, std::size_t maximum) noexcept;
};
class ControlArchive {
public:
    ControlArchiveState& state;
    explicit ControlArchive(ControlArchiveState& context) noexcept : state(context) {}
    virtual ~ControlArchive() = default;
    virtual bool uint_field(unsigned tag, std::uint64_t&) noexcept = 0;
    virtual bool optional_uint_field(unsigned tag, std::uint64_t&, bool& present) noexcept = 0;
    virtual bool int_field(unsigned tag, std::int32_t&) noexcept = 0;
    virtual bool double_field(unsigned tag, double&) noexcept = 0;
    virtual bool bool_field(unsigned tag, bool&) noexcept = 0;
    virtual bool text_field(unsigned tag, std::string_view&, std::size_t maximum) noexcept = 0;
    virtual bool object_field(unsigned tag, ControlSchema, void*, ControlVisit, bool* present = nullptr) noexcept = 0;
    virtual bool sequence_field(unsigned tag, ControlSchema, const void* data,
        std::size_t& count, ControlElementVisit) noexcept = 0;
};
bool visit_control_message(ControlArchive&, void*) noexcept;
unsigned control_schema_max_tag(ControlSchema) noexcept;
bool validate_control_node(const WireNode&, const ControlCodecLimits&) noexcept;
bool control_arena_fits(const ControlStorageRequirements&, ControlDecodeArena) noexcept;
} // namespace detail
} // namespace bdsp_protocol
