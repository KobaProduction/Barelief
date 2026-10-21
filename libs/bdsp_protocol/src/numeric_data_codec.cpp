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
bool dense_extents(const NumericContract &contract,std::size_t &e0,std::size_t &e1,unsigned &rank) {
    rank = contract.shape.rank == Rank::Scalar ? 0 : contract.shape.rank == Rank::Vector ? 1 : 2;
    e0 = 1;
    e1 = 1;
    const auto max = std::numeric_limits<std::size_t>::max();
    if (rank) {
        if (contract.shape.extents[0].maximum > max) return false;
        e0 = static_cast<std::size_t>(contract.shape.extents[0].maximum);
    }
    if (rank == 2) {
        if (contract.shape.extents[1].maximum > max) return false;
        e1 = static_cast<std::size_t>(contract.shape.extents[1].maximum);
    }
    return e0 && e1;
}
bool dense_geometry(const NumericContract& contract,std::size_t available,
                    std::size_t& width,std::size_t& elements,std::size_t& second,
                    unsigned& rank) noexcept {
    if(storage_width(contract,width)!=Status::ok || contract.channel_order.empty() ||
       contract.channel_order.size()>available)return false;
    std::size_t first=0;
    if(!dense_extents(contract,first,second,rank))return false;
    const auto maximum=std::numeric_limits<std::size_t>::max();
    if(first && second>maximum/first)return false;
    elements=first*second;
    if(elements>maximum/width ||
       elements*width>maximum/contract.channel_order.size())return false;
    return true;
}

bool dense_layout(const Message& message,std::span<PlaneLayout> output) {
    const auto* contract=std::get_if<NumericContract>(&message.contract);
    if(!contract)return true;
    std::size_t width=0,elements=0,second=0;
    unsigned rank=0;
    if(!dense_geometry(*contract,output.size(),width,elements,second,rank))return false;
    const auto planes=contract->channel_order.size();
    const auto lane=contract->representation.layout==ChannelLayout::Interleaved?planes*width:width;
    for(std::size_t plane=0;plane<planes;++plane)
        output[plane]={contract->representation.layout==ChannelLayout::Interleaved?
                       plane*width:plane*elements*width,
                       {rank?(rank==2?second*lane:lane):0,rank==2?lane:0}};
    return true;
}
NumericContract history_row_contract(const NumericContract& registered,bool matrix_history) {
    auto row=registered;
    row.semantic=row.history_element;
    row.history_element=Semantic::Numeric;
    if(matrix_history){
        row.shape={Rank::Vector,{registered.shape.axes[1],Axis::None},
            {registered.shape.extents[1],ExtentBounds{}}};
        row.coordinates={registered.coordinates[1],AxisCoordinates{}};
    }else{
        row.shape={Rank::Scalar,{Axis::None,Axis::None},{ExtentBounds{},ExtentBounds{}}};
        row.coordinates={AxisCoordinates{},AxisCoordinates{}};
    }
    return row;
}
DataContract binding_contract(const Binding& b,Kind kind) {
    if(kind!=Kind::HistoryRow)return b.contract;
    const auto* registered=std::get_if<NumericContract>(&b.contract);
    if(!registered)return b.contract;
    const bool matrix_history=registered->shape.rank==Rank::Matrix;
    const bool vector_history=registered->shape.rank==Rank::Vector && b.history_contract.shape.rank==Rank::Vector;
    if(!matrix_history && !vector_history)return b.contract;
    return history_row_contract(*registered,matrix_history);
}
bool matches_dense_planes(const NumericPayload& payload,const NumericContract& contract,
                          std::span<const PlaneLayout> dense) noexcept {
    if(payload.planes.size()!=contract.channel_order.size())return false;
    for(std::size_t index=0;index<payload.planes.size();++index)
        if(payload.planes[index].offset!=dense[index].offset ||
           payload.planes[index].stride_bytes!=dense[index].stride_bytes)return false;
    return true;
}

bool reference_actual_extents(const NumericPayload& payload,const NumericContract& contract) noexcept {
    if(payload.ring.capacity)return true;
    for(unsigned axis=0;axis<2;++axis)
        if(payload.actual[axis]!=contract.shape.extents[axis].maximum)return false;
    return true;
}

bool reference_numeric_layout(const Message& message) {
    const auto* contract=std::get_if<NumericContract>(&message.contract);
    if(!contract)return true;
    const bool marker=message.kind==Kind::Query || message.kind==Kind::Empty ||
                      message.kind==Kind::Reset || message.kind==Kind::Unavailable;
    if(marker)return true;
    std::array<PlaneLayout,16> dense{};
    if(!dense_layout(message,dense) || !matches_dense_planes(message.numeric,*contract,dense))return false;
    std::size_t required=0;
    if(required_storage(*contract,message.numeric.planes,required)!=Status::ok ||
       required!=message.numeric.storage.size())return false;
    return reference_actual_extents(message.numeric,*contract);
}
bool reference_matches(const Message &m, ReferenceContext context) {
    const auto *b = binding_for(m, context);
    if (!b || b->values != m.values || b->endpoint.graph != m.endpoint.graph ||
        b->endpoint.node != m.endpoint.node || b->endpoint.port != m.endpoint.port ||
        b->endpoint.direction != m.endpoint.direction || b->stream != m.stamp.stream_id ||
        !same_contract(binding_contract(*b,m.kind), m.contract)) return false;
    if (m.kind == Kind::HistoryRow &&
        !same_contract(DataContract{b->history_contract}, DataContract{m.history_contract})) return false;
    return reference_numeric_layout(m);
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
    void numeric_contract_shape(NumericContract &c) {
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
    }
    void numeric_contract_representation(NumericContract &c) {
        symbol(c.representation.encoding);
        u(c.representation.valid_bits);
        symbol(c.representation.placement);
        symbol(c.representation.layout);
        symbol(c.representation.normalization);
        size(c.representation.alignment);
    }
    void numeric_contract_coordinates(NumericContract &c) {
        for (auto &axis : c.coordinates) {
            symbol(axis.mode);
            unit(axis.unit);
            ratio(axis.origin);
            ratio(axis.step);
        }
        text(c.clock.domain);
        ratio(c.clock.tick_seconds);
        u(c.clock.sample_rate);
    }
    void transfer_channels(NumericContract& contract,std::uint16_t count) {
        if(reading) {
            auto channels=std::span(plan->channels).subspan(channel_at,count);
            for(auto& channel:channels)u(channel);
            contract.channel_order=channels;
        }else for(auto channel:contract.channel_order)u(channel);
    }
    void numeric_contract_channels(NumericContract &c) {
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
        transfer_channels(c,n);
        channel_at += n;
        req.channels += n;
    }
    void numeric_contract(NumericContract &c) {
        numeric_contract_shape(c);
        numeric_contract_representation(c);
        numeric_contract_coordinates(c);
        numeric_contract_channels(c);
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
    bool reference_binding(Message &m) {
        u(m.reference.binding);
        u(m.reference.contract);
        const auto *b = binding_for(m, context);
        if (!b) { fail(Status::incompatible_contract); return false; }
        if (!reading) return true;
        m.contract = binding_contract(*b,m.kind);
        m.history_contract = m.kind == Kind::HistoryRow ? b->history_contract : NumericContract{};
        m.endpoint = b->endpoint;
        m.stamp.stream_id = b->stream;
        if (m.values != b->values) { fail(Status::incompatible_contract); return false; }
        return true;
    }
    void reference_stamp(Message &m) {
        u(m.stamp.sequence);
        u(m.stamp.generation);
        u(m.stamp.epoch);
        u(m.stamp.graph_revision);
        u(m.stamp.source_sequence);
        u(m.stamp.producer_epoch);
        signed64(m.stamp.timestamp_ticks);
        boolean(m.stamp.available);
        boolean(m.stamp.discontinuity);
        boolean(m.stamp.has_sample_interval);
        signed64(m.stamp.sample_begin);
        u(m.stamp.sample_count);
        u(m.stamp.latency_samples);
        signed64(m.stamp.latency_seconds.numerator);
        u(m.stamp.latency_seconds.denominator);
        boolean(m.stamp.frequency_dependent_latency);
    }
    void reference_history(Message &m) {
        if (m.kind != Kind::Frame) {
            u(m.history.filled);
            u(m.history.capacity);
            u(m.history.ordinal);
        }
    }
    bool reference_numeric_layout(Message &m,const NumericContract &contract) {
        auto &n = m.numeric;
        size(n.actual[0]);
        size(n.actual[1]);
        size(n.ring.capacity);
        size(n.ring.oldest);
        boolean(n.semantic_silence);
        if (reading && m.kind != Kind::Query && m.kind != Kind::Empty && m.kind != Kind::Reset &&
            m.kind != Kind::Unavailable) {
            if (!dense_layout(m, plan->planes)) { fail(Status::overflow); return false; }
            n.planes = std::span(plan->planes).first(contract.channel_order.size());
        }
        return true;
    }
    bool reference_numeric(Message &m,const NumericContract &contract) {
        if (!reference_numeric_layout(m,contract)) return false;
        auto &n = m.numeric;
        blob(n.storage);
        blob(n.ring.silence_mask);
        blob(n.coordinates.axes[0]);
        blob(n.coordinates.axes[1]);
        req.text += contract.clock.domain.size();
        req.channels += contract.channel_order.size();
        req.planes = n.planes.size();
        if (m.kind == Kind::HistoryRow) {
            req.text += m.history_contract.clock.domain.size();
            req.channels += m.history_contract.channel_order.size();
        }
        return true;
    }
    void reference_payload(Message &m) {
        if (const auto *c = std::get_if<NumericContract>(&m.contract)) {
            if (!reference_numeric(m,*c)) return;
        } else {
            const auto &s = std::get<StructuredContract>(m.contract);
            req.text += s.schema.size() + s.encoding.size();
            blob(m.structured);
        }
        req.text += m.endpoint.graph.size() + m.stamp.stream_id.size();
        blob(m.extensions);
    }
    void reference_message(Message &m) {
        if (!reference_binding(m)) return;
        reference_stamp(m);
        reference_history(m);
        reference_payload(m);
    }
    bool message_versions() {
        std::uint32_t magic = 0x5441444e;
        std::uint16_t version = PayloadVersion, grammar = DeliveryVersion;
        u(magic);
        u(version);
        u(grammar);
        if (magic != 0x5441444e) {
            fail();
            return false;
        }
        if (version != PayloadVersion || grammar != DeliveryVersion) {
            fail(Status::unsupported_version);
            return false;
        }
        return true;
    }
    bool message_identity(Message &m,bool &referenced) {
        if(!message_versions())return false;
        symbol(m.kind);
        std::uint8_t flags = m.reference.binding ? 1 : 0;
        u(flags);
        if (flags > 1)
            fail();
        referenced = flags == 1;
        std::uint8_t reserved = 0;
        symbol(m.values);
        u(reserved);
        if (reserved)
            fail();
        return true;
    }
    void message_envelope(Message &m) {
        u(m.envelope.nonce);
        u(m.envelope.session);
        u(m.envelope.delivery_sequence);
        u(m.envelope.delivery_epoch);
        u(m.envelope.delivery_generation);
        u(m.envelope.revision);
        u(m.envelope.request);
    }
    void explicit_endpoint(Message &m) {
        auto node = std::bit_cast<std::uint32_t>(m.endpoint.node);
        u(node);
        if (reading) m.endpoint.node = std::bit_cast<std::int32_t>(node);
        symbol(m.endpoint.direction);
        u(m.endpoint.port);
        text(m.endpoint.graph);
    }
    void explicit_stamp(Message &m) {
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
        boolean(m.stamp.has_sample_interval);
        signed64(m.stamp.sample_begin);
        u(m.stamp.sample_count);
        u(m.stamp.latency_samples);
        signed64(m.stamp.latency_seconds.numerator);
        u(m.stamp.latency_seconds.denominator);
        boolean(m.stamp.frequency_dependent_latency);
    }
    void explicit_plane(NumericPayload& payload,std::size_t index) {
        PlaneLayout plane=reading?PlaneLayout{}:payload.planes[index];
        size(plane.offset);
        size(plane.stride_bytes[0]);
        size(plane.stride_bytes[1]);
        if(reading)plan->planes[index]=plane;
    }
    void explicit_planes(NumericPayload &n) {
        if (n.planes.size() > 16) { fail(Status::exhausted); return; }
        std::uint16_t count = static_cast<std::uint16_t>(n.planes.size());
        u(count);
        if (count > 16) { fail(Status::exhausted); return; }
        if (reading) n.planes = std::span(plan->planes).first(count);
        req.planes = count;
        for(std::size_t index=0;index<count;++index)explicit_plane(n,index);
    }
    void explicit_numeric_payload(NumericPayload &n) {
        size(n.actual[0]);
        size(n.actual[1]);
        size(n.ring.capacity);
        size(n.ring.oldest);
        boolean(n.semantic_silence);
        explicit_planes(n);
        if (status != Status::ok) return;
        blob(n.storage);
        blob(n.ring.silence_mask);
        blob(n.coordinates.axes[0]);
        blob(n.coordinates.axes[1]);
    }
    void explicit_payload(Message &m) {
        contract(m.contract);
        u(m.history.filled);
        u(m.history.capacity);
        u(m.history.ordinal);
        if (m.kind == Kind::HistoryRow) numeric_contract(m.history_contract);
        if (std::holds_alternative<NumericContract>(m.contract)) explicit_numeric_payload(m.numeric);
        else blob(m.structured);
        if (status != Status::ok) return;
        blob(m.extensions);
    }
    void message(Message &m) {
        bool referenced=false;
        if (!message_identity(m,referenced)) return;
        message_envelope(m);
        if (referenced) {
            reference_message(m);
            req.wire = at;
            return;
        }
        explicit_endpoint(m);
        explicit_stamp(m);
        explicit_payload(m);
        req.wire = at;
    }
};
bool metadata_fits(const Message &message,const Requirements &required,const Limits &limits) {
    const auto payload=std::holds_alternative<NumericContract>(message.contract)
        ? message.numeric.storage.size() : message.structured.size();
    const auto metadata=required.bytes-payload+required.text+required.channels*sizeof(std::uint32_t)+
        required.planes*sizeof(PlaneLayout);
    return required.text<=limits.text_bytes && metadata<=limits.frame.max_metadata_bytes;
}
Status read_wire_plan(std::span<const std::byte> wire,Plan &plan,ReferenceContext context,Archive &archive) {
    archive.in=wire;
    archive.reading=true;
    archive.plan=&plan;
    archive.context=context;
    archive.message(plan.message);
    if(archive.status!=Status::ok)return archive.status;
    if(archive.at!=wire.size())return Status::malformed;
    if(plan.message.reference.binding && !reference_matches(plan.message,context))return Status::incompatible_contract;
    return Status::ok;
}
Result preflight(std::span<const std::byte> wire, const Limits &l, Plan &p, ReferenceContext context) {
    if (wire.size() > l.message_bytes)
        return {Status::exhausted, {}};
    Archive a;
    if(const auto status=read_wire_plan(wire,p,context,a);status!=Status::ok)return {status,{}};
    auto s = validate(p.message, l);
    if (s != Status::ok)
        return {s, {}};
    if (!metadata_fits(p.message,a.req,l))
        return {Status::exhausted, {}};
    return {Status::ok, a.req};
}
struct Rebaser {
    Arena arena;
    std::size_t text_at=0,byte_at=0,channel_at=0;
    void text(std::string_view &s) {
        auto n = s.size();
        if (n)
            std::memcpy(arena.text.data() + text_at, s.data(), n);
        s = n ? std::string_view(arena.text.data() + text_at, n) : std::string_view{};
        text_at += n;
    }
    void bytes(std::span<const std::byte> &s) {
        auto n = s.size();
        if (n)
            std::memcpy(arena.bytes.data() + byte_at, s.data(), n);
        s = n ? std::span<const std::byte>(arena.bytes.data() + byte_at, n) : std::span<const std::byte>{};
        byte_at += n;
    }
    void numeric_contract(NumericContract &c) {
        text(c.clock.domain);
        const auto n = c.channel_order.size();
        std::copy(c.channel_order.begin(), c.channel_order.end(), arena.channels.begin() + channel_at);
        c.channel_order = arena.channels.subspan(channel_at, n);
        channel_at += n;
    }
    void numeric_payload(Message &m,NumericContract &contract) {
        numeric_contract(contract);
        if (m.kind == Kind::HistoryRow) numeric_contract(m.history_contract);
        std::copy(m.numeric.planes.begin(), m.numeric.planes.end(), arena.planes.begin());
        m.numeric.planes = arena.planes.first(m.numeric.planes.size());
        bytes(m.numeric.storage);
        bytes(m.numeric.ring.silence_mask);
        bytes(m.numeric.coordinates.axes[0]);
        bytes(m.numeric.coordinates.axes[1]);
    }
    void structured_payload(Message &m) {
        auto &structured = std::get<StructuredContract>(m.contract);
        text(structured.schema);
        text(structured.encoding);
        bytes(m.structured);
    }
    void apply(Message &m) {
        text(m.endpoint.graph);
        text(m.stamp.stream_id);
        if (auto *contract = std::get_if<NumericContract>(&m.contract)) numeric_payload(m,*contract);
        else structured_payload(m);
        bytes(m.extensions);
    }
};
void rebase(Message &m, Arena arena) {
    Rebaser{arena}.apply(m);
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
    if (a.req.wire > l.message_bytes || !metadata_fits(m,a.req,l))
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
