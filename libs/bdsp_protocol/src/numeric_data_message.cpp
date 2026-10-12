#include <bdsp_protocol/numeric_data_message.h>
namespace bdsp_protocol::numeric_data {
using namespace signal_contracts;
namespace {
template <class E> struct Symbol {
    E value;
    std::uint8_t id;
};
template <class E, std::size_t N> bool get_id(E value, std::uint8_t &out, const Symbol<E> (&table)[N]) {
    for (auto p : table)
        if (p.value == value) {
            out = p.id;
            return true;
        }
    return false;
}
template <class E, std::size_t N> bool get_value(std::uint8_t id, E &out, const Symbol<E> (&table)[N]) {
    for (auto p : table)
        if (p.id == id) {
            out = p.value;
            return true;
        }
    return false;
}
} // namespace
#define SYMBOLS(E, NAME, ...)                                                                                \
    namespace {                                                                                              \
    constexpr Symbol<E> NAME[]{__VA_ARGS__};                                                                 \
    }                                                                                                        \
    bool symbol_id(E v, std::uint8_t &out) noexcept { return get_id(v, out, NAME); }                         \
    bool symbol_value(std::uint8_t id, E &out) noexcept { return get_value(id, out, NAME); }
SYMBOLS(DType, dtypes, {DType::Int8, 1}, {DType::UInt8, 2}, {DType::Int16, 3}, {DType::UInt16, 4},
        {DType::Int32, 5}, {DType::UInt32, 6}, {DType::Int64, 7}, {DType::UInt64, 8}, {DType::Float32, 9},
        {DType::Float64, 10})
SYMBOLS(Rank, ranks, {Rank::Scalar, 0}, {Rank::Vector, 1}, {Rank::Matrix, 2})
SYMBOLS(Semantic, semantics, {Semantic::Scalar, 1}, {Semantic::PCM, 2}, {Semantic::Window, 3},
        {Semantic::Windowed, 4}, {Semantic::SpectrumMagnitude, 5}, {Semantic::MelEnergy, 6},
        {Semantic::History, 7}, {Semantic::Numeric, 8})
SYMBOLS(Axis, axes, {Axis::None, 0}, {Axis::Sample, 1}, {Axis::Bin, 2}, {Axis::Band, 3}, {Axis::Time, 4},
        {Axis::Declared, 5})
SYMBOLS(Quantity, quantities, {Quantity::Unspecified, 0}, {Quantity::Amplitude, 1},
        {Quantity::SpectrumMagnitude, 2}, {Quantity::MelEnergy, 3}, {Quantity::FilledFrameCount, 4},
        {Quantity::Packet, 5})
SYMBOLS(NumericUnit, units, {NumericUnit::Unspecified, 0}, {NumericUnit::NormalizedAmplitude, 1},
        {NumericUnit::RawAmplitude, 2}, {NumericUnit::Frames, 3}, {NumericUnit::Hertz, 4},
        {NumericUnit::Seconds, 5}, {NumericUnit::Values, 6})
SYMBOLS(StorageEncoding, encodings, {StorageEncoding::FixedLittleEndian, 1},
        {StorageEncoding::FixedBigEndian, 2}, {StorageEncoding::PackedSigned24LE, 3},
        {StorageEncoding::PackedSigned24BE, 4})
SYMBOLS(BitPlacement, placements, {BitPlacement::LeastSignificant, 1}, {BitPlacement::MostSignificant, 2})
SYMBOLS(ChannelLayout, layouts, {ChannelLayout::Planar, 1}, {ChannelLayout::Interleaved, 2})
SYMBOLS(Normalization, normalizations, {Normalization::None, 0}, {Normalization::SignedUnit, 1},
        {Normalization::UnsignedUnit, 2}, {Normalization::FloatUnit, 3})
SYMBOLS(CoordinateMode, coordinates, {CoordinateMode::None, 0}, {CoordinateMode::Regular, 1},
        {CoordinateMode::ExplicitFloat64LE, 2})
SYMBOLS(ValueDomain, domains, {ValueDomain::FiniteOnly, 1}, {ValueDomain::AnyIeee, 2})
SYMBOLS(Kind, kinds, {Kind::Frame, 1}, {Kind::HistoryRow, 2}, {Kind::Snapshot, 3}, {Kind::Query, 4},
        {Kind::Empty, 5}, {Kind::Reset, 6}, {Kind::Unavailable, 7})
SYMBOLS(Direction, directions, {Direction::Input, 1}, {Direction::Output, 2})
#undef SYMBOLS

Negotiated negotiate(std::uint16_t a, std::uint16_t b, std::uint32_t x, std::uint32_t y) noexcept {
    if (a != PayloadVersion || b != PayloadVersion || ((x | y) & ~15U))
        return {};
    auto f = x & y;
    if (!(f & 1U))
        return {};
    if ((f & 12U) != 12U)
        f &= ~12U;
    return {PayloadVersion, f};
}
signal_contracts::DataFrameView Message::frame_view() const noexcept {
    using namespace signal_contracts;
    if (const auto *c = std::get_if<NumericContract>(&contract))
        return {stamp, NumericView{c, numeric.storage, numeric.planes, numeric.actual, numeric.ring,
                                   numeric.coordinates, numeric.semantic_silence}};
    return {stamp, StructuredView{&std::get<StructuredContract>(contract), structured}};
}
namespace {
bool text_valid(std::string_view s, std::size_t limit, bool empty = false) noexcept {
    if ((s.empty() && !empty) || s.size() > limit)
        return false;
    // Strict UTF-8 (no overlong, surrogates, NUL, or out-of-range code points).
    for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i++]);
        if (!c)
            return false;
        if (c < 128)
            continue;
        unsigned n = 0;
        std::uint32_t value = 0, minimum = 0;
        if (c >= 194 && c <= 223) {
            n = 1;
            value = c & 31;
            minimum = 128;
        } else if (c >= 224 && c <= 239) {
            n = 2;
            value = c & 15;
            minimum = 2048;
        } else if (c >= 240 && c <= 244) {
            n = 3;
            value = c & 7;
            minimum = 65536;
        } else
            return false;
        if (n > s.size() - i)
            return false;
        while (n--) {
            auto t = static_cast<unsigned char>(s[i++]);
            if ((t & 192) != 128)
                return false;
            value = (value << 6) | (t & 63);
        }
        if (value < minimum || value > 1114111 || (value >= 55296 && value <= 57343))
            return false;
    }
    return true;
}
} // namespace
signal_contracts::Status validate(const Message &m, const Limits &l) noexcept {
    using namespace signal_contracts;
    if (!m.stamp.latency_seconds.denominator || m.stamp.latency_seconds.numerator<0)
        return Status::invalid_argument;
    if (bool(m.reference.binding) != bool(m.reference.contract))
        return Status::invalid_argument;
    if (l.frame.max_elements > 800000 || l.frame.max_payload_bytes > 800000 ||
        l.frame.max_metadata_bytes > 65536 || l.frame.max_string_bytes > 256 ||
        l.frame.max_coordinate_bytes > 32768 || l.text_bytes > 65536 || l.extension_bytes > 4096 ||
        l.history_rows > 2000 || l.identity_bytes > 256)
        return Status::exhausted;
    if (!l.message_bytes || l.message_bytes > 900000 || l.frame.max_planes > 16 ||
        !text_valid(m.endpoint.graph, l.identity_bytes) || m.endpoint.node < 0 ||
        (m.endpoint.direction != Direction::Input && m.endpoint.direction != Direction::Output) ||
        !m.envelope.nonce || !m.envelope.session || !m.envelope.delivery_sequence ||
        !text_valid(m.stamp.stream_id, l.identity_bytes) ||
        (m.values != ValueDomain::FiniteOnly && m.values != ValueDomain::AnyIeee))
        return Status::invalid_argument;
    if (m.kind != Kind::Frame && m.kind != Kind::HistoryRow && m.kind != Kind::Snapshot &&
        m.kind != Kind::Query && m.kind != Kind::Empty && m.kind != Kind::Reset &&
        m.kind != Kind::Unavailable)
        return Status::invalid_argument;
    if ((m.kind == Kind::Query || m.kind == Kind::Snapshot) && !m.envelope.request)
        return Status::invalid_argument;
    if (m.extensions.size() > l.extension_bytes)
        return Status::exhausted;
    for (std::size_t at = 0; at < m.extensions.size();) {
        if (m.extensions.size() - at < 6)
            return Status::malformed;
        const auto *b = m.extensions.data() + at;
        auto tag = std::to_integer<unsigned>(b[0]) | (std::to_integer<unsigned>(b[1]) << 8);
        std::uint32_t size = 0;
        for (unsigned i = 0; i < 4; ++i)
            size |= std::uint32_t(std::to_integer<unsigned>(b[2 + i])) << (8 * i);
        if (tag < 1024 || (tag & 32768))
            return Status::malformed;
        for (std::size_t prior = 0; prior < at;) {
            auto previous = std::to_integer<unsigned>(m.extensions[prior]) |
                            (std::to_integer<unsigned>(m.extensions[prior + 1]) << 8);
            if (previous == tag)
                return Status::malformed;
            std::uint32_t length = 0;
            for (unsigned i = 0; i < 4; ++i)
                length |= std::uint32_t(std::to_integer<unsigned>(m.extensions[prior + 2 + i])) << (8 * i);
            prior += 6 + length;
        }
        at += 6;
        if (size > m.extensions.size() - at)
            return Status::malformed;
        at += size;
    }
    auto s = validate_contract(m.contract, l.frame);
    if (s != Status::ok)
        return s;
    const auto *c = std::get_if<NumericContract>(&m.contract);
    // Ring snapshots are explicit wire operations; passive semantic tags do not select them.
    if (m.kind == Kind::Snapshot && (!c || c->shape.rank != Rank::Matrix))
        return Status::incompatible_contract;
    auto contract_text = [&](const NumericContract &n) {
        auto known = [](auto value) {
            std::uint8_t id = 0;
            return symbol_id(value, id);
        };
        bool valid = known(n.dtype) && known(n.shape.rank) && known(n.semantic) && known(n.history_element) &&
                     known(n.quantity) && known(n.unit) && known(n.representation.encoding) &&
                     known(n.representation.placement) && known(n.representation.layout) &&
                     known(n.representation.normalization);
        for (unsigned axis = 0; axis < 2; ++axis)
            valid = valid && known(n.shape.axes[axis]) && known(n.coordinates[axis].mode) &&
                    known(n.coordinates[axis].unit);
        return valid && text_valid(n.clock.domain, l.frame.max_string_bytes, true) &&
               n.representation.alignment <= 8;
    };
    if (c && !contract_text(*c))
        return Status::invalid_argument;
    if (!c) {
        const auto &t = std::get<StructuredContract>(m.contract);
        if (!text_valid(t.schema, l.frame.max_string_bytes) ||
            !text_valid(t.encoding, l.frame.max_string_bytes))
            return Status::invalid_argument;
        if (t.schema == "wled-audio-sync" && (t.version != 1 || t.encoding != "audio-sync-44-le" ||
                                              t.payload_bytes.minimum != 44 || t.payload_bytes.maximum != 44))
            return Status::incompatible_contract;
        if (m.kind != Kind::Frame && m.kind != Kind::Unavailable)
            return Status::incompatible_contract;
        if (!m.numeric.storage.empty() || !m.numeric.planes.empty() || !m.numeric.ring.silence_mask.empty() ||
            !m.numeric.coordinates.axes[0].empty() || !m.numeric.coordinates.axes[1].empty())
            return Status::invalid_argument;
    } else if (!m.structured.empty())
        return Status::invalid_argument;
    if (m.kind == Kind::Unavailable) {
        if (m.stamp.available || !m.numeric.storage.empty() || !m.structured.empty() ||
            !m.numeric.planes.empty() || m.numeric.ring.capacity || m.numeric.ring.oldest ||
            !m.numeric.ring.silence_mask.empty() || !m.numeric.coordinates.axes[0].empty() ||
            !m.numeric.coordinates.axes[1].empty() || m.numeric.semantic_silence)
            return Status::invalid_argument;
    } else if (!m.stamp.available)
        return Status::invalid_argument;
    const bool marker = m.kind == Kind::Query || m.kind == Kind::Empty || m.kind == Kind::Reset;
    if (marker) {
        if (!c || c->shape.rank != Rank::Matrix || !m.numeric.storage.empty() ||
            !m.numeric.planes.empty() || m.numeric.ring.capacity || m.numeric.ring.oldest ||
            m.numeric.semantic_silence || !m.numeric.ring.silence_mask.empty() ||
            !m.numeric.coordinates.axes[0].empty() || !m.numeric.coordinates.axes[1].empty() ||
            (m.kind == Kind::Query && !m.envelope.request))
            return Status::invalid_argument;
    } else if (m.kind != Kind::Unavailable) {
        if (c) {
            // Absolute wire address need not align. Validate declared offset/stride alignment
            // separately, then use alignment1 ONLY on this temporary read-only wire view.
            for (const auto &p : m.numeric.planes)
                if (p.offset % c->representation.alignment ||
                    p.stride_bytes[0] % c->representation.alignment ||
                    p.stride_bytes[1] % c->representation.alignment)
                    return Status::invalid_argument;
            auto relaxed = *c;
            relaxed.representation.alignment = 1;
            auto f = m.frame_view();
            auto &v = std::get<NumericView>(f.payload);
            v.contract = &relaxed;
            s = validate_view(f, l.frame);
            if (s != Status::ok)
                return s;
            s = validate_values(v, m.values, l.frame);
            if (s != Status::ok)
                return s;
        } else {
            s = validate_view(m.frame_view(), l.frame);
            if (s != Status::ok)
                return s;
        }
    }
    if (c && (m.kind == Kind::Snapshot || marker || (m.kind == Kind::Unavailable && m.history.capacity))) {
        if(c->shape.rank!=Rank::Matrix)return Status::incompatible_contract;
        if (m.kind != Kind::Snapshot && !marker && m.kind != Kind::Unavailable)
            return Status::incompatible_contract;
        if (!l.history_rows || c->shape.extents[0].maximum > l.history_rows)
            return Status::exhausted;
        if (m.history.capacity != c->shape.extents[0].maximum || m.history.filled > m.history.capacity ||
            m.history.ordinal < m.history.filled)
            return Status::invalid_argument;
        if (m.kind == Kind::Snapshot &&
            (m.numeric.actual[0] != m.history.capacity || m.history.filled > m.numeric.actual[0] || m.numeric.ring.capacity != m.history.capacity))
            return Status::invalid_argument;
        if ((m.kind == Kind::Empty || m.kind == Kind::Reset) && m.history.filled)
            return Status::invalid_argument;
    }
    if (m.kind == Kind::HistoryRow) {
        if (!c || c->shape.rank != Rank::Vector || m.numeric.ring.capacity || m.numeric.ring.oldest ||
            !m.numeric.ring.silence_mask.empty())
            return Status::incompatible_contract;
        s = validate_contract(DataContract{m.history_contract}, l.frame);
        if (s != Status::ok)
            return s;
        const auto &h = m.history_contract;
        if (!contract_text(h) || h.shape.rank != Rank::Matrix || !l.history_rows ||
            m.history.capacity != h.shape.extents[0].maximum || m.history.capacity > l.history_rows ||
            !m.history.filled || m.history.filled > m.history.capacity ||
            m.history.ordinal < m.history.filled)
            return Status::invalid_argument;
        auto row = h;
        row.semantic = h.history_element;
        row.history_element = Semantic::Numeric;
        row.shape.rank = Rank::Vector;
        row.shape.axes = {h.shape.axes[1], Axis::None};
        row.shape.extents = {h.shape.extents[1], ExtentBounds{}};
        row.coordinates = {h.coordinates[1], AxisCoordinates{}};
        if (!same_physical_contract(DataContract{row}, m.contract))
            return Status::incompatible_contract;
        std::size_t width = 0;
        s = storage_width(*c, width);
        if (s != Status::ok)
            return s;
        if (m.numeric.actual[0] > l.frame.max_payload_bytes / width / c->channel_order.size() ||
            m.numeric.storage.size() != m.numeric.actual[0] * width * c->channel_order.size())
            return Status::invalid_argument;
    } else if ((m.kind == Kind::Frame || (m.kind == Kind::Unavailable && !m.history.capacity)) && (m.history.filled || m.history.capacity || m.history.ordinal))
        return Status::invalid_argument;
    if(m.kind==Kind::Frame && m.numeric.ring.capacity)return Status::incompatible_contract;
    if (m.kind != Kind::HistoryRow && !m.history_contract.channel_order.empty())
        return Status::invalid_argument;
    if (!marker && m.kind != Kind::Unavailable && !m.stamp.sequence)
        return Status::invalid_argument;
    return Status::ok;
}
} // namespace bdsp_protocol::numeric_data
