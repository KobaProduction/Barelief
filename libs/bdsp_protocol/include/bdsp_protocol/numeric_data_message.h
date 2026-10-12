#pragma once
#include <bdsp_protocol/numeric_data_limits.h>
namespace bdsp_protocol::numeric_data {
inline constexpr std::uint8_t SubscribeId = 26, AckId = 27, EventsId = 28, QueryId = 29, SnapshotId = 30,
                              FragmentId = 31;
inline constexpr std::uint16_t PayloadVersion = 5, DeliveryVersion = 1;
enum class Kind { Frame, HistoryRow, Snapshot, Query, Empty, Reset, Unavailable };
enum class Direction { Input, Output };
enum class Feature : std::uint32_t { Numeric = 1, Structured = 2, Rows = 4, Query = 8 };
// One shared symbolic field table for DATA and future K control serializers.
// No ordinal casts. Unknown symbols/IDs fail without changing the destination.
#define BDSP_NUMERIC_SYMBOL(T)                                                                               \
    bool symbol_id(T, std::uint8_t &) noexcept;                                                              \
    bool symbol_value(std::uint8_t, T &) noexcept;
BDSP_NUMERIC_SYMBOL(signal_contracts::DType)
BDSP_NUMERIC_SYMBOL(signal_contracts::Rank)
BDSP_NUMERIC_SYMBOL(signal_contracts::Semantic)
BDSP_NUMERIC_SYMBOL(signal_contracts::Axis)
BDSP_NUMERIC_SYMBOL(signal_contracts::Quantity)
BDSP_NUMERIC_SYMBOL(signal_contracts::NumericUnit)
BDSP_NUMERIC_SYMBOL(signal_contracts::StorageEncoding)
BDSP_NUMERIC_SYMBOL(signal_contracts::BitPlacement)
BDSP_NUMERIC_SYMBOL(signal_contracts::ChannelLayout)
BDSP_NUMERIC_SYMBOL(signal_contracts::Normalization)
BDSP_NUMERIC_SYMBOL(signal_contracts::CoordinateMode)
BDSP_NUMERIC_SYMBOL(signal_contracts::ValueDomain)
BDSP_NUMERIC_SYMBOL(Kind)
BDSP_NUMERIC_SYMBOL(Direction)
#undef BDSP_NUMERIC_SYMBOL
inline constexpr std::string_view feature_names[]{"numeric-data-v1", "structured-data-v1",
                                                  "history-row-events-v1", "history-snapshot-query-v1"};
struct Negotiated {
    std::uint16_t payload = 0;
    std::uint32_t features = 0;
};
Negotiated negotiate(std::uint16_t local_version, std::uint16_t peer_version, std::uint32_t local_features,
                     std::uint32_t peer_features) noexcept;
struct Endpoint {
    std::string_view graph;
    std::int32_t node = -1;
    Direction direction = Direction::Output;
    std::uint16_t port = 0;
};
struct Envelope {
    std::uint64_t nonce = 0, session = 0, delivery_sequence = 0, delivery_epoch = 0, delivery_generation = 0,
                  revision = 0, request = 0;
};
struct History {
    std::uint64_t filled = 0, capacity = 0, ordinal = 0;
};
struct NumericPayload {
    std::span<const std::byte> storage;
    std::span<const signal_contracts::PlaneLayout> planes;
    std::array<std::size_t, 2> actual{1, 1};
    signal_contracts::RingRows ring;
    signal_contracts::CoordinateView coordinates;
    bool semantic_silence = false;
};
struct Reference {
    std::uint32_t binding = 0, contract = 0;
};
struct Binding {
    Reference reference;
    Endpoint endpoint;
    std::string_view stream;
    signal_contracts::DataContract contract;
    signal_contracts::NumericContract history_contract;
    signal_contracts::ValueDomain values = signal_contracts::ValueDomain::FiniteOnly;
};
// Caller supplies immutable selected bindings from positive negotiation. Not a registry.
struct ReferenceContext {
    std::uint64_t nonce = 0, session = 0, revision = 0;
    std::span<const Binding> bindings;
};
// Values owned by DTO; variable data borrowed. No pointer to a DTO's own contract.
// frame_view() rebases after copy/move; arena/pool owner must outlive every view.
struct Message {
    Kind kind = Kind::Frame;
    Endpoint endpoint;
    Envelope envelope;
    signal_contracts::DataContract contract;
    signal_contracts::FrameStamp stamp;
    signal_contracts::ValueDomain values = signal_contracts::ValueDomain::FiniteOnly;
    NumericPayload numeric;
    std::span<const std::byte> structured;
    signal_contracts::NumericContract history_contract;
    History history;
    Reference reference;
    // Raw optional extension TLVs: u16 tag1024..32767, u32 length, bytes.
    // High bit=unknown mandatory -> refusal; duplicate/shadow tags refused.
    std::span<const std::byte> extensions;
    // Query/Empty/Reset are control markers, not complete NumericViews. Their
    // admitted History contract/count is meaningful without a physical ring.
    bool has_payload() const noexcept {
        return stamp.available && (kind == Kind::Frame || kind == Kind::HistoryRow || kind == Kind::Snapshot);
    }
    signal_contracts::DataFrameView frame_view() const noexcept;
};
signal_contracts::Status validate(const Message &, const Limits &) noexcept;
} // namespace bdsp_protocol::numeric_data
