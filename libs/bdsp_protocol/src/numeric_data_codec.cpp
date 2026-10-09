#include <algorithm>
#include <bdsp_protocol/numeric_data_codec.h>
#include <bit>
#include <cstring>
#include <limits>
namespace bdsp_protocol::numeric_data {
namespace {
using namespace signal_contracts;
struct Plan {
    Message message;
    std::array<std::uint32_t, 32> channels{};
    std::array<PlaneLayout, 16> planes{};
};
const Binding *binding_for(const Message &m, ReferenceContext context) {
    if (!m.reference.binding || !m.reference.contract || context.bindings.size() > 256 ||
        context.nonce != m.envelope.nonce || context.session != m.envelope.session ||
        context.revision != m.envelope.revision)
        return nullptr;
    const Binding *found = nullptr;
    for (const auto &binding : context.bindings)
        if (binding.reference.binding == m.reference.binding) {
            if (found || binding.reference.contract != m.reference.contract)
                return nullptr;
            found = &binding;
        }
    return found;
}
bool dense_layout(const Message &m, std::span<PlaneLayout> out) {
    const auto *c = std::get_if<NumericContract>(&m.contract);
    if (!c)
        return true;
    std::size_t w = 0;
    if (storage_width(*c, w) != Status::ok || c->channel_order.empty() ||
        c->channel_order.size() > out.size())
        return false;
    const auto rank = c->shape.rank == Rank::Scalar ? 0 : c->shape.rank == Rank::Vector ? 1 : 2;
    std::size_t e0 = 1, e1 = 1;
    const auto max = std::numeric_limits<std::size_t>::max();
    if (rank) {
        if (c->shape.extents[0].maximum > max)
            return false;
        e0 = static_cast<std::size_t>(c->shape.extents[0].maximum);
    }
    if (rank == 2) {
        if (c->shape.extents[1].maximum > max)
            return false;
        e1 = static_cast<std::size_t>(c->shape.extents[1].maximum);
    }
    if (!e0 || !e1)
        return false;
    const auto planes = c->channel_order.size();
    if (e0 && e1 > max / e0)
        return false;
    auto elements = e0 * e1;
    if (elements > max / w || elements * w > max / planes)
        return false;
    const auto lane = c->representation.layout == ChannelLayout::Interleaved ? planes * w : w;
    for (std::size_t p = 0; p < planes; ++p)
        out[p] = {c->representation.layout == ChannelLayout::Interleaved ? p * w : p * elements * w,
                  {rank ? (rank == 2 ? e1 * lane : lane) : 0, rank == 2 ? lane : 0}};
    return true;
}
DataContract binding_contract(const Binding& b,Kind kind) {
    if(kind==Kind::HistoryRow)if(const auto* matrix=std::get_if<NumericContract>(&b.contract);matrix && matrix->shape.rank==Rank::Matrix){
        auto row=*matrix;row.semantic=row.history_element;row.history_element=Semantic::Numeric;
        row.shape={Rank::Vector,{matrix->shape.axes[1],Axis::None},{matrix->shape.extents[1],ExtentBounds{}}};
        row.coordinates={matrix->coordinates[1],AxisCoordinates{}};return row;
    }
    return b.contract;
}
bool reference_matches(const Message &m, ReferenceContext context) {
    const auto *b = binding_for(m, context);
    if (!b || b->values != m.values || b->endpoint.graph != m.endpoint.graph ||
        b->endpoint.node != m.endpoint.node || b->endpoint.port != m.endpoint.port ||
        b->endpoint.direction != m.endpoint.direction || b->stream != m.stamp.stream_id ||
        !same_contract(binding_contract(*b,m.kind), m.contract))
        return false;
    if (m.kind == Kind::HistoryRow &&
        !same_contract(DataContract{b->history_contract}, DataContract{m.history_contract}))
        return false;
    const auto *c = std::get_if<NumericContract>(&m.contract);
    if (!c)
        return true;
    const bool marker = m.kind == Kind::Query || m.kind == Kind::Empty || m.kind == Kind::Reset ||
                        m.kind == Kind::Unavailable;
    if (marker)
        return true;
    std::array<PlaneLayout, 16> dense{};
    if (!dense_layout(m, dense) || m.numeric.planes.size() != c->channel_order.size())
        return false;
    for (std::size_t i = 0; i < m.numeric.planes.size(); ++i)
        if (m.numeric.planes[i].offset != dense[i].offset ||
            m.numeric.planes[i].stride_bytes != dense[i].stride_bytes)
            return false;
    std::size_t required = 0;
    if (required_storage(*c, m.numeric.planes, required) != Status::ok ||
        required != m.numeric.storage.size())
        return false;
    if (!m.numeric.ring.capacity)
        for (unsigned i = 0; i < 2; ++i)
            if (m.numeric.actual[i] != c->shape.extents[i].maximum)
                return false;
    return true;
}

struct Archive {
    std::span<const std::byte> in;
    std::span<std::byte> out;
    bool reading = false;
    std::size_t at = 0, channel_at = 0;
    Status status = Status::ok;
    Requirements req;
    Plan *plan = nullptr;
    ReferenceContext context;
    void fail(Status s = Status::malformed) {
        if (status == Status::ok)
            status = s;
    }
    bool room(std::size_t n) {
        if (status != Status::ok)
            return false;
        if (n > std::numeric_limits<std::size_t>::max() - at) {
            fail(Status::overflow);
            return false;
        }
        if (reading && n > in.size() - std::min(at, in.size())) {
            fail();
            return false;
        }
        if (!reading && !out.empty() && n > out.size() - std::min(at, out.size())) {
            fail(Status::buffer_too_small);
            return false;
        }
        return true;
    }
    template <class T> void u(T &x) {
        static_assert(std::is_unsigned_v<T>);
        if (!room(sizeof(T)))
            return;
        if (reading) {
            x = 0;
            for (unsigned i = 0; i < sizeof(T); ++i)
                x |= T(std::to_integer<unsigned>(in[at + i])) << (8 * i);
        } else if (!out.empty())
            for (unsigned i = 0; i < sizeof(T); ++i)
                out[at + i] = std::byte((x >> (8 * i)) & 255);
        at += sizeof(T);
    }
    void boolean(bool &b) {
        std::uint8_t x = b ? 1 : 0;
        u(x);
        if (x > 1)
            fail();
        if (reading)
            b = x == 1;
    }
    void signed64(std::int64_t &x) {
        auto b = std::bit_cast<std::uint64_t>(x);
        u(b);
        if (reading)
            x = std::bit_cast<std::int64_t>(b);
    }
    void size(std::size_t &n) {
        std::uint64_t x = n;
        u(x);
        if (x > std::numeric_limits<std::size_t>::max()) {
            fail(Status::overflow);
            return;
        }
        if (reading)
            n = static_cast<std::size_t>(x);
    }
    template <class E> void symbol(E &e) {
        std::uint8_t x = 0;
        if (!reading && !symbol_id(e, x)) {
            fail(Status::invalid_argument);
            return;
        }
        u(x);
        if (reading && !symbol_value(x, e))
            fail();
    }
    void text(std::string_view &s) {
        if (s.size() > 65535) {
            fail(Status::exhausted);
            return;
        }
        std::uint16_t n = static_cast<std::uint16_t>(s.size());
        u(n);
        if (!room(n))
            return;
        if (reading)
            s = {reinterpret_cast<const char *>(in.data() + at), n};
        else if (!out.empty() && n)
            std::memcpy(out.data() + at, s.data(), n);
        at += n;
        req.text += n;
    }
    void blob(std::span<const std::byte> &b) {
        if (b.size() > UINT32_MAX) {
            fail(Status::overflow);
            return;
        }
        std::uint32_t n = static_cast<std::uint32_t>(b.size());
        u(n);
        if (!room(n))
            return;
        if (reading)
            b = in.subspan(at, n);
        else if (!out.empty() && n)
            std::memcpy(out.data() + at, b.data(), n);
        at += n;
        req.bytes += n;
    }
    void ratio(Ratio &r) {
        signed64(r.numerator);
        u(r.denominator);
    }
    void unit(NumericUnit &x) { symbol(x); }
    void semantic(Semantic &x) { symbol(x); }
    void numeric_contract(NumericContract &c) {
        symbol(c.dtype);
        symbol(c.shape.rank);
        semantic(c.semantic);
        semantic(c.history_element);
        symbol(c.quantity);
        unit(c.unit);
        for (unsigned i = 0; i < 2; ++i) {
            symbol(c.shape.axes[i]);
            u(c.shape.extents[i].minimum);
            u(c.shape.extents[i].maximum);
        }
        symbol(c.representation.encoding);
        u(c.representation.valid_bits);
        symbol(c.representation.placement);
        symbol(c.representation.layout);
        symbol(c.representation.normalization);
        size(c.representation.alignment);
        for (auto &axis : c.coordinates) {
            symbol(axis.mode);
            unit(axis.unit);
            ratio(axis.origin);
            ratio(axis.step);
        }
        text(c.clock.domain);
        ratio(c.clock.tick_seconds);
        u(c.clock.sample_rate);
        if (c.channel_order.size() > 16) {
            fail(Status::exhausted);
            return;
        }
        std::uint16_t n = static_cast<std::uint16_t>(c.channel_order.size());
        u(n);
        if (n > 16 || n > 32 - channel_at) {
            fail(Status::exhausted);
            return;
        }
        if (reading) {
            auto span = std::span(plan->channels).subspan(channel_at, n);
            for (auto &x : span)
                u(x);
            c.channel_order = span;
        } else
            for (auto value : c.channel_order)
                u(value);
        channel_at += n;
        req.channels += n;
    }
    void contract(DataContract &data) {
        std::uint8_t type = std::holds_alternative<NumericContract>(data) ? 1 : 2;
        u(type);
        if (type == 1) {
            if (reading)
                data = NumericContract{};
            numeric_contract(std::get<NumericContract>(data));
        } else if (type == 2) {
            if (reading)
                data = StructuredContract{};
            auto &s = std::get<StructuredContract>(data);
            text(s.schema);
            u(s.version);
            text(s.encoding);
            u(s.payload_bytes.minimum);
            u(s.payload_bytes.maximum);
        } else
            fail();
    }
    void reference_message(Message &m) {
        u(m.reference.binding);
        u(m.reference.contract);
        const auto *b = binding_for(m, context);
        if (!b) {
            fail(Status::incompatible_contract);
            return;
        }
        if (reading) {
            m.contract = binding_contract(*b,m.kind);
            m.history_contract = m.kind == Kind::HistoryRow ? b->history_contract : NumericContract{};
            m.endpoint = b->endpoint;
            m.stamp.stream_id = b->stream;
            if (m.values != b->values) {
                fail(Status::incompatible_contract);
                return;
            }
        }
        u(m.stamp.sequence);
        u(m.stamp.generation);
        u(m.stamp.epoch);
        u(m.stamp.graph_revision);
        u(m.stamp.source_sequence);
        u(m.stamp.producer_epoch);
        signed64(m.stamp.timestamp_ticks);
        boolean(m.stamp.available);
        boolean(m.stamp.discontinuity);
        if (m.kind != Kind::Frame) {
            u(m.history.filled);
            u(m.history.capacity);
            u(m.history.ordinal);
        }
        if (const auto *c = std::get_if<NumericContract>(&m.contract)) {
            auto &n = m.numeric;
            size(n.actual[0]);
            size(n.actual[1]);
            size(n.ring.capacity);
            size(n.ring.oldest);
            boolean(n.semantic_silence);
            if (reading && m.kind != Kind::Query && m.kind != Kind::Empty && m.kind != Kind::Reset &&
                m.kind != Kind::Unavailable) {
                if (!dense_layout(m, plan->planes)) {
                    fail(Status::overflow);
                    return;
                }
                n.planes = std::span(plan->planes).first(c->channel_order.size());
            }
            blob(n.storage);
            blob(n.ring.silence_mask);
            blob(n.coordinates.axes[0]);
            blob(n.coordinates.axes[1]);
            req.text += c->clock.domain.size();
            req.channels += c->channel_order.size();
            req.planes = n.planes.size();
            if (m.kind == Kind::HistoryRow) {
                req.text += m.history_contract.clock.domain.size();
                req.channels += m.history_contract.channel_order.size();
            }
        } else {
            const auto &s = std::get<StructuredContract>(m.contract);
            req.text += s.schema.size() + s.encoding.size();
            blob(m.structured);
        }
        req.text += m.endpoint.graph.size() + m.stamp.stream_id.size();
        blob(m.extensions);
    }
    void message(Message &m) {
        std::uint32_t magic = 0x5441444e;
        std::uint16_t version = PayloadVersion, grammar = DeliveryVersion;
        u(magic);
        u(version);
        u(grammar);
        if (magic != 0x5441444e) {
            fail();
            return;
        }
        if (version != PayloadVersion || grammar != DeliveryVersion) {
            fail(Status::unsupported_version);
            return;
        }
        symbol(m.kind);
        std::uint8_t flags = m.reference.binding ? 1 : 0;
        u(flags);
        if (flags > 1)
            fail();
        const bool referenced = flags == 1;
        std::uint8_t reserved = 0;
        symbol(m.values);
        u(reserved);
        if (reserved)
            fail();
        u(m.envelope.nonce);
        u(m.envelope.session);
        u(m.envelope.delivery_sequence);
        u(m.envelope.delivery_epoch);
        u(m.envelope.delivery_generation);
        u(m.envelope.revision);
        u(m.envelope.request);
        if (referenced) {
            reference_message(m);
            req.wire = at;
            return;
        }
        auto node = std::bit_cast<std::uint32_t>(m.endpoint.node);
        u(node);
        if (reading)
            m.endpoint.node = std::bit_cast<std::int32_t>(node);
        symbol(m.endpoint.direction);
        u(m.endpoint.port);
        text(m.endpoint.graph);
        u(m.stamp.sequence);
        u(m.stamp.generation);
        u(m.stamp.epoch);
        u(m.stamp.graph_revision);
        u(m.stamp.source_sequence);
        u(m.stamp.producer_epoch);
        signed64(m.stamp.timestamp_ticks);
        text(m.stamp.stream_id);
        boolean(m.stamp.available);
        boolean(m.stamp.discontinuity);
        contract(m.contract);
        u(m.history.filled);
        u(m.history.capacity);
        u(m.history.ordinal);
        if (m.kind == Kind::HistoryRow)
            numeric_contract(m.history_contract);
        if (std::holds_alternative<NumericContract>(m.contract)) {
            auto &n = m.numeric;
            size(n.actual[0]);
            size(n.actual[1]);
            size(n.ring.capacity);
            size(n.ring.oldest);
            boolean(n.semantic_silence);
            if (n.planes.size() > 16) {
                fail(Status::exhausted);
                return;
            }
            std::uint16_t count = static_cast<std::uint16_t>(n.planes.size());
            u(count);
            if (count > 16) {
                fail(Status::exhausted);
                return;
            }
            if (reading)
                n.planes = std::span(plan->planes).first(count);
            req.planes = count;
            for (std::size_t i = 0; i < count; ++i) {
                PlaneLayout p = reading ? PlaneLayout{} : n.planes[i];
                size(p.offset);
                size(p.stride_bytes[0]);
                size(p.stride_bytes[1]);
                if (reading)
                    plan->planes[i] = p;
            }
            blob(n.storage);
            blob(n.ring.silence_mask);
            blob(n.coordinates.axes[0]);
            blob(n.coordinates.axes[1]);
        } else
            blob(m.structured);
        blob(m.extensions);
        req.wire = at;
    }
};
Result preflight(std::span<const std::byte> wire, const Limits &l, Plan &p, ReferenceContext context) {
    if (wire.size() > l.message_bytes)
        return {Status::exhausted, {}};
    Archive a;
    a.in = wire;
    a.reading = true;
    a.plan = &p;
    a.context = context;
    a.message(p.message);
    if (a.status != Status::ok)
        return {a.status, {}};
    if (a.at != wire.size())
        return {Status::malformed, {}};
    if (p.message.reference.binding && !reference_matches(p.message, context))
        return {Status::incompatible_contract, {}};
    auto s = validate(p.message, l);
    if (s != Status::ok)
        return {s, {}};
    const auto payload = std::holds_alternative<NumericContract>(p.message.contract)
                             ? p.message.numeric.storage.size()
                             : p.message.structured.size();
    const auto metadata = a.req.bytes - payload + a.req.text + a.req.channels * sizeof(std::uint32_t) +
                          a.req.planes * sizeof(PlaneLayout);
    if (a.req.text > l.text_bytes || metadata > l.frame.max_metadata_bytes)
        return {Status::exhausted, {}};
    return {Status::ok, a.req};
}
void rebase(Message &m, Arena arena) {
    std::size_t text_at = 0, byte_at = 0, channel_at = 0;
    auto text = [&](std::string_view &s) {
        auto n = s.size();
        if (n)
            std::memcpy(arena.text.data() + text_at, s.data(), n);
        s = n ? std::string_view(arena.text.data() + text_at, n) : std::string_view{};
        text_at += n;
    };
    auto bytes = [&](std::span<const std::byte> &s) {
        auto n = s.size();
        if (n)
            std::memcpy(arena.bytes.data() + byte_at, s.data(), n);
        s = n ? std::span<const std::byte>(arena.bytes.data() + byte_at, n) : std::span<const std::byte>{};
        byte_at += n;
    };
    auto nc = [&](NumericContract &c) {
        text(c.clock.domain);
        const auto n = c.channel_order.size();
        std::copy(c.channel_order.begin(), c.channel_order.end(), arena.channels.begin() + channel_at);
        c.channel_order = arena.channels.subspan(channel_at, n);
        channel_at += n;
    };
    text(m.endpoint.graph);
    text(m.stamp.stream_id);
    if (auto *c = std::get_if<NumericContract>(&m.contract)) {
        nc(*c);
        if (m.kind == Kind::HistoryRow)
            nc(m.history_contract);
        std::copy(m.numeric.planes.begin(), m.numeric.planes.end(), arena.planes.begin());
        m.numeric.planes = arena.planes.first(m.numeric.planes.size());
        bytes(m.numeric.storage);
        bytes(m.numeric.ring.silence_mask);
        bytes(m.numeric.coordinates.axes[0]);
        bytes(m.numeric.coordinates.axes[1]);
    } else {
        auto &structured = std::get<StructuredContract>(m.contract);
        text(structured.schema);
        text(structured.encoding);
        bytes(m.structured);
    }
    bytes(m.extensions);
}
} // namespace
Result measure(const Message &m, const Limits &l, ReferenceContext context) noexcept {
    if (m.reference.binding && !reference_matches(m, context))
        return {signal_contracts::Status::incompatible_contract, {}};
    auto s = validate(m, l);
    if (s != signal_contracts::Status::ok)
        return {s, {}};
    auto copy = m;
    Archive a;
    a.context = context;
    a.message(copy);
    if (a.status != signal_contracts::Status::ok)
        return {a.status, {}};
    const auto payload = std::holds_alternative<signal_contracts::NumericContract>(m.contract)
                             ? m.numeric.storage.size()
                             : m.structured.size();
    const auto metadata = a.req.bytes - payload + a.req.text + a.req.channels * sizeof(std::uint32_t) +
                          a.req.planes * sizeof(signal_contracts::PlaneLayout);
    if (a.req.wire > l.message_bytes || a.req.text > l.text_bytes || metadata > l.frame.max_metadata_bytes)
        return {signal_contracts::Status::exhausted, {}};
    return {signal_contracts::Status::ok, a.req};
}
Result encode(const Message &m, std::span<std::byte> output, std::span<std::byte> scratch, const Limits &l,
              ReferenceContext context) noexcept {
    auto r = measure(m, l, context);
    if (!r)
        return r;
    if (output.size() < r.required.wire || scratch.size() < r.required.wire) {
        r.status = signal_contracts::Status::buffer_too_small;
        return r;
    }
    auto copy = m;
    Archive a;
    a.context = context;
    a.out = scratch.first(r.required.wire);
    a.message(copy);
    if (a.status != signal_contracts::Status::ok) {
        r.status = a.status;
        return r;
    }
    std::memcpy(output.data(), scratch.data(), r.required.wire);
    r.produced = r.required.wire;
    return r;
}
Result inspect(std::span<const std::byte> wire, const Limits &l, ReferenceContext context) noexcept {
    Plan p;
    return preflight(wire, l, p, context);
}
Result decode(std::span<const std::byte> wire, Message &output, Arena arena, const Limits &l,
              ReferenceContext context) noexcept {
    Plan p;
    auto r = preflight(wire, l, p, context);
    if (!r)
        return r;
    if (arena.bytes.size() < r.required.bytes || arena.text.size() < r.required.text ||
        arena.channels.size() < r.required.channels || arena.planes.size() < r.required.planes) {
        r.status = signal_contracts::Status::buffer_too_small;
        return r;
    }
    if (r.required.bytes && reinterpret_cast<std::uintptr_t>(arena.bytes.data()) % 8) {
        r.status = signal_contracts::Status::invalid_argument;
        return r;
    }
    rebase(p.message, arena);
    output = p.message;
    r.consumed = wire.size();
    return r;
}
} // namespace bdsp_protocol::numeric_data
