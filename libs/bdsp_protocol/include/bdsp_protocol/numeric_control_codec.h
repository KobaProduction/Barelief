#pragma once
#include <bdsp_protocol/numeric_control_message.h>
namespace bdsp_protocol::numeric_control {
// All storage stable/disjoint for call. Arenas aligned to alignof(max_align_t).
// Decode scratch is disposable; destination arena/message are unchanged on error.
// required_scratch on decode is the staging arena byte count. If staging is short,
// capacity reports describe structural footprint, not domain admission. With
// sufficient staging the complete message is validated before destination sizing.
// No owning domain construction, allocation, exceptions, RTTI or implicit lowering.
class Codec {
  public:
    virtual ~Codec() = default;
    virtual CodecId id() const noexcept = 0;
    // Validates and sizes the canonical encoding without allocating or writing.
    // On success required_wire/required_scratch size encode storage;
    // required_arena sizes each decode arena (destination and staging).
    // required_text counts aggregate text/extension bytes against total_text_bytes.
    // Limits still apply; error results do not promise complete capacity counts.
    Result measure(const Message &, const Limits &) const noexcept;
    virtual Result encode(const Message &, std::span<std::uint8_t> output, std::span<std::uint8_t> scratch,
                          const Limits &) const noexcept = 0;
    virtual Result decode(std::span<const std::uint8_t> input, std::span<std::byte> arena, std::span<std::byte> staging,
                          Message &, const Limits &) const noexcept = 0;
};
class CborCodec final : public Codec {
  public:
    CodecId id() const noexcept override {
        return CodecId::Cbor;
    }
    Result encode(const Message &, std::span<std::uint8_t>, std::span<std::uint8_t>,
                  const Limits &) const noexcept override;
    Result decode(std::span<const std::uint8_t>, std::span<std::byte>, std::span<std::byte>, Message &,
                  const Limits &) const noexcept override;
};
class ProtobufCodec final : public Codec {
  public:
    CodecId id() const noexcept override {
        return CodecId::Protobuf;
    }
    Result encode(const Message &, std::span<std::uint8_t>, std::span<std::uint8_t>,
                  const Limits &) const noexcept override;
    Result decode(std::span<const std::uint8_t>, std::span<std::byte>, std::span<std::byte>, Message &,
                  const Limits &) const noexcept override;
};
namespace detail {
enum class Schema {
    Literal,
    Ratio,
    Extent,
    Coordinates,
    Representation,
    Numeric,
    Structured,
    Contract,
    Policy,
    Dyadic,
    Parameter,
    Parameters,
    Oscillator,
    MixerWeight,
    Port,
    Node,
    Edge,
    Versions,
    Graph,
    Command,
    Resources,
    CapabilityRow,
    Capabilities,
    Diagnostic,
    State,
    Reply,
    Extension,
    Message,
    UIntElement
};
struct Context {
    const Limits &limits;
    std::span<std::byte> arena;
    bool reading = false, materializing = false;
    Status error = Status::Ok;
    std::size_t used = 0, text = 0, records = 0, binary_bytes = 0;
    bool validating = false;
    bool fail(Status = Status::Malformed) noexcept;
    void *reserve(std::size_t count, std::size_t width, std::size_t alignment) noexcept;
    bool store(std::string_view &, std::size_t maximum, bool binary = false) noexcept;
};
class Archive;
using Visit = bool (*)(Archive &, void *) noexcept;
using ElementVisit = bool (*)(Archive &, void *, std::size_t) noexcept;
class Archive {
  public:
    Context &state;
    explicit Archive(Context &s) : state(s) {}
    virtual ~Archive() = default;
    virtual bool uint_field(unsigned, std::uint64_t &) noexcept = 0;
    virtual bool int_field(unsigned, std::int64_t &) noexcept = 0;
    virtual bool bool_field(unsigned, bool &) noexcept = 0;
    virtual bool text_field(unsigned, std::string_view &, bool binary = false) noexcept = 0;
    virtual bool optional_uint(unsigned, std::uint64_t &, bool &) noexcept = 0;
    virtual bool optional_int(unsigned, std::int64_t &, bool &) noexcept = 0;
    virtual bool object(unsigned, Schema, void *, Visit, bool *present = nullptr) noexcept = 0;
    virtual bool sequence_size(unsigned, std::size_t &) noexcept = 0;
    virtual bool sequence(unsigned, Schema, void *, std::size_t, ElementVisit) noexcept = 0;
};
bool visit_message(Archive &, void *) noexcept;
unsigned max_tag(Schema) noexcept;
} // namespace detail
} // namespace bdsp_protocol::numeric_control
