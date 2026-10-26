#include <algorithm>
#include <array>
#include <bdsp_protocol/numeric_data_codec.h>
#include <cstdio>
#include <cstdlib>
#include <new>
using namespace signal_contracts;
using namespace bdsp_protocol::numeric_data;
int failures = 0;
std::size_t checks = 0;
bool observe_allocations = false;
std::size_t allocations = 0;
void *operator new(std::size_t n) {
    if (observe_allocations)
        ++allocations;
    auto *p = std::malloc(n ? n : 1);
    if (!p)
        std::abort();
    return p;
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void check(bool b, const char *name) {
    ++checks;
    if (!b) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}
namespace {
struct Buffers {
    alignas(8) std::array<std::byte, 16384> wire{}, scratch{}, bytes{};
    std::array<char, 4096> text{};
    std::array<std::uint32_t, 32> channels{};
    std::array<PlaneLayout, 16> planes{};
    Message decoded;
    Result roundtrip(const Message &m, const Limits &l = desktop_limits()) {
        auto e = encode(m, wire, scratch, l);
        check(bool(e), "matrix encode");
        if (!e)
            return e;
        auto r = inspect(std::span(wire).first(e.produced), l);
        check(bool(r) && r.required.wire == e.produced, "inspect exact requirements");
        auto d = decode(std::span(wire).first(e.produced), decoded, {bytes, text, channels, planes}, l);
        check(bool(d) && d.consumed == e.produced, "matrix decode");
        return d;
    }
};
Message base(const NumericContract &c, std::span<const std::byte> storage,
             std::span<const PlaneLayout> planes) {
    Message m;
    m.contract = c;
    m.endpoint = {"main", 0, Direction::Input, 0};
    m.envelope = {1, 2, 3, 4, 5, 6, 0};
    m.stamp.sequence = 7;
    m.stamp.stream_id = "x";
    m.numeric.storage = storage;
    m.numeric.planes = planes;
    return m;
}
} // namespace
void reference_literal_golden(const Message& m,Buffers& b,const ReferenceContext& context,const auto& e){
    std::array<std::byte, 2277> golden{};
    const std::array<std::byte, 12> prefix{std::byte{0x4e}, std::byte{0x44}, std::byte{0x41}, std::byte{0x54},
                                           std::byte{6},    std::byte{0},    std::byte{1},    std::byte{0},
                                           std::byte{1},    std::byte{1},    std::byte{1},    std::byte{0}};
    std::copy(prefix.begin(), prefix.end(), golden.begin());
    for (auto [at, value] : {std::pair{12, 1},
                             {20, 2},
                             {28, 3},
                             {36, 4},
                             {44, 5},
                             {52, 6},
                             {68, 9},
                             {72, 11},
                             {76, 7},
                             {132, 1},
                             {167, 1},
                             {176, 1},
                             {183, 1},
                             {209, 8}})
        golden[at>=175?at+1:at] = std::byte(value);
    check(std::equal(golden.begin(), golden.end(), b.wire.begin()),
          "complete literal stereo256 reference golden");
    auto d = decode(std::span(b.wire).first(e.produced), b.decoded, {b.bytes, b.text, b.channels, b.planes},
                    device_block_limits(), context);
    check(bool(d) && same_contract(b.decoded.contract, m.contract) && b.decoded.endpoint.graph == "main" &&
              b.decoded.stamp.stream_id == "x",
          "reference decoder canonical metadata");
    check(d && validate_view(b.decoded.frame_view(), device_block_limits().frame) == Status::ok,
          "reference canonical R view");
}
void reference_context_refusals(const Message& m,Buffers& b,const Binding& binding,const ReferenceContext& context,const std::array<PlaneLayout,2>& planes,const auto& e){
    for (std::size_t n = 0; n < e.produced; ++n)
        check(!inspect(std::span(b.wire).first(n), device_block_limits(), context),
              "reference every prefix truncation refuses");
    b.bytes.fill(std::byte{31});
    b.text.fill('q');
    b.channels.fill(77);
    auto out_before = b.decoded;
    for (unsigned case_id = 0; case_id < 5; ++case_id) {
        auto bad_context = context;
        auto bad_binding = binding;
        if (case_id == 0)
            ++bad_context.session;
        if (case_id == 1)
            ++bad_context.revision;
        if (case_id == 2)
            ++bad_binding.reference.contract;
        if (case_id == 3)
            ++bad_binding.reference.binding;
        if (case_id == 4)
            bad_binding.values = ValueDomain::AnyIeee;
        bad_context.bindings = std::span(&bad_binding, 1);
        auto r = decode(std::span(b.wire).first(e.produced), b.decoded,
                        {b.bytes, b.text, b.channels, b.planes}, device_block_limits(), bad_context);
        check(!r && !r.consumed && b.bytes[0] == std::byte{31} && b.text[0] == 'q' && b.channels[0] == 77 &&
                  b.decoded.envelope.session == out_before.envelope.session,
              "reference context failure atomic");
    }
    check(!decode(std::span(b.wire).first(e.produced), b.decoded, {b.bytes, b.text, b.channels, b.planes},
                  device_block_limits()),
          "reference missing negotiated table refuses");
    auto bad = m;
    bad.endpoint.port = 1;
    check(!measure(bad, device_block_limits(), context), "reference changed endpoint refuses");
    std::array<PlaneLayout, 2> bad_planes = planes;
    bad_planes[1].offset = 1028;
    bad = m;
    bad.numeric.planes = bad_planes;
    check(!measure(bad, device_block_limits(), context), "reference noncanonical stride/layout refuses");
    std::array<Binding, 2> duplicate{binding, binding};
    auto duplicated = context;
    duplicated.bindings = duplicate;
    check(!measure(m, device_block_limits(), duplicated), "duplicate binding context refuses");
}
void boundary_capacities(Message& m,Buffers& b,const auto& e){
    auto oversized = m;
    std::array<PlaneLayout, 1> huge{{{SIZE_MAX, {SIZE_MAX, SIZE_MAX}}}};
    oversized.numeric.planes = huge;
    check(!measure(oversized, desktop_limits()), "checked plane/stride overflow");
    auto tight = desktop_limits();
    tight.frame.max_payload_bytes = 7;
    check(!measure(m, tight), "payload profile refusal");
    tight = desktop_limits();
    tight.frame.max_metadata_bytes = 1;
    check(!measure(m, tight), "metadata aggregate refusal");
    tight = desktop_limits();
    tight.text_bytes = 1;
    check(!measure(m, tight), "aggregate text refusal");
    for (unsigned which = 0; which < 4; ++which) {
        auto r = inspect(std::span(b.wire).first(e.produced), desktop_limits());
        auto arena = Arena{b.bytes, b.text, b.channels, b.planes};
        if (which == 0)
            arena.bytes = arena.bytes.first(r.required.bytes - 1);
        if (which == 1)
            arena.text = arena.text.first(r.required.text - 1);
        if (which == 2)
            arena.channels = arena.channels.first(r.required.channels - 1);
        if (which == 3)
            arena.planes = arena.planes.first(r.required.planes - 1);
        b.bytes.fill(std::byte{17});
        b.text.fill('q');
        auto d = decode(std::span(b.wire).first(e.produced), b.decoded, arena, desktop_limits());
        check(!d && d.status == Status::buffer_too_small && !d.consumed && d.required.wire == e.produced,
              "each one-short arena capacity");
        check(b.bytes[0] == std::byte{17} && b.text[0] == 'q', "one-short no writes");
    }
    b.wire.fill(std::byte{0xaa});
    auto r = encode(m, std::span(b.wire).first(e.produced - 1), b.scratch, desktop_limits());
    check(!r && !r.produced && b.wire[0] == std::byte{0xaa}, "encode one-short output atomic");
    r = encode(m, b.wire, std::span(b.scratch).first(e.produced - 1), desktop_limits());
    check(!r && b.wire[0] == std::byte{0xaa}, "encode one-short scratch atomic");
    auto bad = m;
    std::get<NumericContract>(bad.contract).representation.encoding = StorageEncoding::Native;
    check(!measure(bad, desktop_limits()), "Native no wire ID");
    bad = m;
    bad.endpoint.graph = std::string_view("\xc0\x80", 2);
    check(!measure(bad, desktop_limits()), "overlong UTF8");
    bad = m;
    bad.endpoint.direction = static_cast<Direction>(33);
    check(!measure(bad, desktop_limits()), "invalid direction");
    for (auto malformed : {DType(99)}) {
        bad = m;
        std::get<NumericContract>(bad.contract).dtype = malformed;
        check(!measure(bad, desktop_limits()), "unknown dtype");
    }
}
void boundary_extensions(Message& m,Buffers& b){
    // Optional exact-byte TLV, unknown mandatory and shadow/duplicate refusal.
    std::array<std::byte, 7> optional{std::byte{0}, std::byte{4}, std::byte{1}, std::byte{0},
                                      std::byte{0}, std::byte{0}, std::byte{83}};
    m.extensions = optional;
    check(bool(b.roundtrip(m)) && b.decoded.extensions.back() == std::byte{83},
          "optional extension retained");
    optional[1] = std::byte{128};
    check(!measure(m, desktop_limits()), "mandatory unknown extension");
    optional[1] = std::byte{0};
    check(!measure(m, desktop_limits()), "critical field shadow extension");
    optional[1] = std::byte{4};
    std::array<std::byte, 14> duplicate{};
    std::copy(optional.begin(), optional.end(), duplicate.begin());
    std::copy(optional.begin(), optional.end(), duplicate.begin() + 7);
    m.extensions = duplicate;
    check(!measure(m, desktop_limits()), "duplicate extension refused");
    m.extensions = {};
}
void boundary_ieee_values(Message& m,Buffers& b,std::array<std::byte,8>& payload){
    // IEEE NaN payload preserved under explicit AnyIeee, finite-only refuses.
    for (unsigned i = 0; i < 8; ++i)
        payload[i] = std::byte((0x7ff8000000000123ULL >> (8 * i)) & 255);
    check(!measure(m, desktop_limits()), "finite-only NaN refused");
    m.values = ValueDomain::AnyIeee;
    check(bool(b.roundtrip(m)) &&
              std::equal(payload.begin(), payload.end(), b.decoded.numeric.storage.begin()),
          "AnyIeee exact NaN bits");
    m.values = ValueDomain::FiniteOnly;
    payload.fill(std::byte{0});
    for (auto bits : {0x8000000000000000ULL, 1ULL, 0x7fefffffffffffffULL}) {
        for (unsigned i = 0; i < 8; ++i)
            payload[i] = std::byte((bits >> (8 * i)) & 255);
        check(bool(b.roundtrip(m)), "finite IEEE negative-zero/subnormal/max");
        std::uint64_t actual = 0;
        check(load_bits(std::get<NumericView>(b.decoded.frame_view().payload), {}, DType::Float64, actual) ==
                      Status::ok &&
                  actual == bits,
              "IEEE finite golden bits");
    }
    payload.fill(std::byte{0});
}
void boundary_retained_ownership(Message& m,Buffers& b,std::array<std::uint32_t,1>& ch){
    // Complete decoded ownership before source release, and explicit R pool handoff.
    check(bool(b.roundtrip(m)), "ownership decode");
    alignas(8) std::array<std::byte, 2048> pp{}, mp{};
    std::array<BufferSlot, 1> pslots{}, mslots{};
    BufferPool ppool(pp, pslots, 2048), mpool(mp, mslots, 2048);
    OwnedDataFrame owner;
    check(retain_frame(b.decoded.frame_view(), desktop_limits().frame, ppool, mpool, owner) == Status::ok,
          "async explicit R retention");
    b.wire.fill(std::byte{255});
    b.bytes.fill(std::byte{255});
    b.text.fill('!');
    ch[0] = 66;
    Message copied = b.decoded;
    check(validate_view(owner.view(), desktop_limits().frame) == Status::ok &&
              owner.view().stamp.stream_id == "x",
          "retained owner independent of wire/arena/donor");
    (void)copied;
}
void boundary_structured_negotiation(Message& m,Buffers& b){
    // Structured contract separate from primitive UInt8. Generic codec preserves bytes;
    // known device adapter remains authority for fields inside WLED packet.
    Message structured = m;
    structured.contract = StructuredContract{"wled-audio-sync", 1, "audio-sync-44-le", {44, 44}};
    structured.numeric = {};
    std::array<std::byte, 44> packet{};
    structured.structured = packet;
    check(bool(b.roundtrip(structured)) &&
              std::holds_alternative<StructuredView>(b.decoded.frame_view().payload),
          "WLED structured category");
    std::get<StructuredContract>(structured.contract).version = 2;
    check(!measure(structured, desktop_limits()), "known WLED schema version mismatch");
    // Pure negotiation intersection does not create a session or promise support.
    check(negotiate(PayloadVersion, PayloadVersion, 15, 9).features == 1, "row/query pair required");
    check(negotiate(PayloadVersion, PayloadVersion, 15, 15).features == 15, "positive feature intersection");
    check(!negotiate(PayloadVersion, PayloadVersion, 16, 15).payload, "unknown feature bit refusal");
    std::uint8_t id = 44;
    DType dtype = DType::Int8;
    check(symbol_id(DType::UInt64, id) && id == 8 && symbol_value(8, dtype) && dtype == DType::UInt64,
          "shared explicit dtype table");
    check(!symbol_value(99, dtype) && dtype == DType::UInt64, "symbol failure unchanged");
}
void boundary_cases();
void history_record_markers(Message& m,Buffers& b){
    m.numeric.actual[0]=3;m.history.filled=1;
    check(bool(b.roundtrip(m)) && b.decoded.numeric.actual[0]==3 && b.decoded.history.filled==1,
          "full capacity snapshot preserves real occupancy independently");
    m.history.filled=0;m.history.ordinal=0;
    check(bool(b.roundtrip(m)) && b.decoded.history.filled==0 && b.decoded.numeric.actual[0]==3,
          "empty padded snapshot retains capacity without inventing rows");
    m.history.filled=4;m.history.ordinal=7;
    check(!measure(m,desktop_limits()),"snapshot occupancy beyond capacity refuses atomically");
    m.history={2,3,7};m.numeric.actual[0]=2;
    m.numeric.ring.oldest = 2;
    m.kind = Kind::Query;
    m.envelope.request = 99;
    m.numeric = {};
    check(bool(b.roundtrip(m)) && b.decoded.envelope.request == 99, "explicit query no history payload");
    m.envelope.request = 0;
    check(!measure(m, desktop_limits()), "query missing request");
    m.envelope.request = 99;
    m.kind = Kind::Empty;
    m.history.filled = 0;
    check(bool(b.roundtrip(m)), "empty count0 distinct no row");
    m.kind = Kind::Reset;
    check(bool(b.roundtrip(m)), "reset distinct marker");
    m.kind = Kind::Unavailable;
    m.stamp.available = false;
    check(bool(b.roundtrip(m)), "known unavailable without payload");
}
void history_reference_row(Message& m,Buffers& b,const NumericContract& row,const NumericContract& h,std::array<std::byte,36>& ring,const std::array<PlaneLayout,2>& rowplanes){
    // All explicit History record variants also use an exact registered binding.
    m.kind = Kind::HistoryRow;
    m.stamp.available = true;
    m.contract = row;
    m.history_contract = h;
    m.numeric = {};
    m.numeric.storage = std::span(ring).first(12);
    m.numeric.planes = rowplanes;
    m.numeric.actual = {3, 1};
    m.history = {2, 3, 7};
    m.reference = {5, 6};
    Binding binding{m.reference, m.endpoint, m.stamp.stream_id, m.contract, h, m.values};
    ReferenceContext context{1, 2, 6, std::span(&binding, 1)};
    auto e = encode(m, b.wire, b.scratch, desktop_limits(), context);
    check(bool(e), "reference compact row encode");
    if (e) {
        auto d = decode(std::span(b.wire).first(e.produced), b.decoded,
                        {b.bytes, b.text, b.channels, b.planes}, desktop_limits(), context);
        check(bool(d) && same_contract(DataContract{b.decoded.history_contract}, DataContract{h}),
              "reference row deep history contract");
    }
}
void compact_history_row_seed(Message& m,Buffers& b,const NumericContract& h,const NumericContract& row,std::array<std::byte,36>& ring,const std::array<PlaneLayout,2>& rowplanes){
    m = base(row, std::span(ring).first(12), rowplanes);
    m.numeric.actual = {3, 1};
    m.kind = Kind::HistoryRow;
    m.history_contract = h;
    m.history_contract.semantic=Semantic::Numeric;
    m.history = {2, 3, 9007199254740993ULL};

    check(bool(b.roundtrip(m)) && b.decoded.history.ordinal == 9007199254740993ULL,
          "compact row exact ordinal/count/contract");
    m.numeric.storage = ring;
    check(!measure(m, desktop_limits()), "row refuses full backing masquerade");
    m.numeric.storage = std::span(ring).first(12);
    std::get<NumericContract>(m.contract).dtype = DType::UInt16;
    check(!measure(m, desktop_limits()), "row history dtype contradiction");
    m.contract = row;
    m = base(h, ring, {});
}
void history_cases();
void music_contract_cases();

Message exact_uint64_message(std::span<const std::uint32_t> channels,
                            std::span<const PlaneLayout> planes,
                            std::span<const std::byte> source) {
    NumericContract c;
    c.dtype = DType::UInt64;
    c.channel_order = channels;
    c.representation.encoding = StorageEncoding::FixedLittleEndian;
    c.clock.domain = "event";
    Message m;
    m.contract = c;
    m.endpoint = {"main", 2147483647, Direction::Output, 65535};
    m.envelope = {1, 2, 3, 4, 5, 6, 0};
    m.stamp.sequence = UINT64_MAX;
    m.stamp.source_sequence = 9007199254740993ULL;
    m.stamp.stream_id = "exact";
    m.stamp.has_sample_interval=true;m.stamp.sample_begin=9007199254740993LL;m.stamp.sample_count=256;m.stamp.latency_samples=63;m.stamp.latency_seconds={1,700};
    m.stamp.frequency_dependent_latency=true;
    m.numeric.storage = source;
    m.numeric.planes = planes;
    return m;
}
void assert_exact_uint64(const Message& out) {
    check(out.stamp.frequency_dependent_latency,"frequency-dependent delay survives DATA codec");
    std::uint64_t value = 0;
    check(load_element(std::get<NumericView>(out.frame_view().payload), {}, value) == Status::ok &&
              value == UINT64_MAX, "exact UInt64 max");
    check(out.stamp.source_sequence == 9007199254740993ULL, "exact lineage >2^53");
    check(out.stamp.has_sample_interval&&out.stamp.sample_begin==9007199254740993LL&&
          out.stamp.sample_count==256&&out.stamp.latency_samples==63&&
          out.stamp.latency_seconds.numerator==1&&out.stamp.latency_seconds.denominator==700,
          "full DATA5 exact sample interval and latency ratio");
}
void exact_uint64_case() {
    std::array<std::uint32_t, 1> channels{17};
    std::array<PlaneLayout, 1> planes{};
    std::array<std::byte, 8> source;
    source.fill(std::byte{255});
    const auto m=exact_uint64_message(channels,planes,source);
    std::array<std::byte, 8192> wire{}, scratch{}, bytes{};
    std::array<char, 2048> text{};
    std::array<std::uint32_t, 32> cs{};
    std::array<PlaneLayout, 16> ps{};
    auto e = encode(m, wire, scratch, desktop_limits());
    check(bool(e), "UInt64 encode");
    if (e) {
        Message out;
        auto d = decode(std::span(wire).first(e.produced), out, {bytes, text, cs, ps}, desktop_limits());
        check(bool(d), "UInt64 decode");
        if (d) assert_exact_uint64(out);
    }
}
void reference_cases();
void snapshot_contract_cases();
void matrix_cases();
int main() {
    exact_uint64_case();
    check(negotiate(PayloadVersion, PayloadVersion-1, 15, 15).payload == 0, "no legacy negotiation fallback");
    observe_allocations = true;
    matrix_cases();
    boundary_cases();
    history_cases();
    reference_cases();
    snapshot_contract_cases();
    music_contract_cases();
    observe_allocations = false;
    check(allocations == 0, "public codec/malformed/ownership paths no heap allocations");
    std::printf("observed allocations=%zu\n", allocations);
    std::printf("numeric DATA checks=%zu failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}


void music_contract_cases() {
    std::array<std::uint32_t,1> channels{0};
    std::array<PlaneLayout,1> planes{{{0,{8,4}}}};
    alignas(8) std::array<std::byte,16> payload{};
    NumericContract contract;contract.dtype=DType::Float32;contract.semantic=Semantic::Numeric;
    contract.channel_order=channels;contract.clock={"music",{1,32768},32768};
    contract.representation.encoding=StorageEncoding::FixedLittleEndian;
    contract.shape={Rank::Matrix,{Axis::Bin,Axis::Component},{ExtentBounds{2,2},ExtentBounds{2,2}}};
    for(int quantity=int(Quantity::SpectrumComplex);quantity<=int(Quantity::SpectrumWindowScaledMagnitude);++quantity)
        for(const auto unit:{NumericUnit::Radians,NumericUnit::SquaredAmplitude,NumericUnit::SquaredAmplitudePerHertz,NumericUnit::Decibels}) {
            contract.quantity=Quantity(quantity);contract.unit=unit;
            auto message=base(contract,payload,planes);message.numeric.actual={2,2};Buffers buffers;
            if(buffers.roundtrip(message)) {
                const auto& decoded=std::get<NumericContract>(buffers.decoded.contract);
                check(decoded.shape.axes[1]==Axis::Component && decoded.quantity==contract.quantity && decoded.unit==unit,
                      "music component/quantity/unit exact roundtrip");
            }
        }
}
void primitive_endpoints(DType dtype,std::size_t width,MutableNumericView& v,Message& m,Buffers& b){
                if (!dtype_descriptor(dtype)->ieee) {
                    const auto width_mask = width == 8 ? UINT64_MAX : (std::uint64_t{1} << (width * 8)) - 1;
                    const auto sign_bit = std::uint64_t{1} << (width * 8 - 1);
                    for (auto endpoint :
                         {std::uint64_t{0}, dtype_descriptor(dtype)->signed_value ? sign_bit : width_mask,
                          dtype_descriptor(dtype)->signed_value ? sign_bit - 1 : std::uint64_t{1}}) {
                        check(store_bits(v, {}, dtype, endpoint) == Status::ok, "integer endpoint store");
                        if (b.roundtrip(m)) {
                            std::uint64_t value = 0;
                            check(load_bits(std::get<NumericView>(b.decoded.frame_view().payload), {}, dtype,
                                            value) == Status::ok &&
                                      value == endpoint,
                                  "all signed/unsigned endpoints exact");
                        }
                    }
                }
}
void primitive_matrix_case(DType dtype, Rank rank, StorageEncoding encoding) {
                const auto width = dtype_descriptor(dtype)->bytes;
                const std::size_t count = rank == Rank::Scalar ? 1 : 3, elements = rank == Rank::Scalar   ? 1
                                                                                   : rank == Rank::Vector ? 2
                                                                                                          : 6;
                std::array<std::uint32_t, 3> channels{1, 4, 9};
                std::array<PlaneLayout, 3> planes{};
                alignas(8) std::array<std::byte, 512> payload{};
                NumericContract c;
                c.dtype = dtype;
                c.shape.rank = rank;
                c.semantic = Semantic::History; // Passive label must not change ordinary Frame transport.
                c.channel_order = std::span(channels).first(count);
                c.representation.encoding = encoding;
                c.representation.alignment = width;
                c.clock.domain = "sample";
                if (rank != Rank::Scalar) {
                    c.shape.axes[0] = Axis::Declared;
                    c.shape.extents[0] = {2, 2};
                }
                if (rank == Rank::Matrix) {
                    c.shape.axes[1] = Axis::Declared;
                    c.shape.extents[1] = {3, 3};
                }
                for (std::size_t p = 0; p < count; ++p) {
                    planes[p].offset = p * elements * width;
                    if (rank == Rank::Vector)
                        planes[p].stride_bytes[0] = width;
                    if (rank == Rank::Matrix)
                        planes[p].stride_bytes = {std::size_t(3) * width, width};
                }
                auto m = base(c, std::span(payload).first(elements * width * count),
                              std::span(planes).first(count));
                m.numeric.actual = {rank == Rank::Scalar ? 1U : 2U, rank == Rank::Matrix ? 3U : 1U};
                MutableNumericView v{&c,
                                     std::span(payload).first(elements * width * count),
                                     m.numeric.planes,
                                     m.numeric.actual,
                                     {},
                                     {}};
                const auto bits = dtype == DType::Float32   ? 0x80000000ULL
                                  : dtype == DType::Float64 ? 0x8000000000000000ULL
                                  : width == 8              ? UINT64_MAX
                                                            : (std::uint64_t{1} << (width * 8)) - 1;
                for (std::size_t p = 0; p < count; ++p)
                    check(store_bits(v, {p, 0, 0}, dtype, bits) == Status::ok, "literal store");
                Buffers b;
                if (!b.roundtrip(m))
                    return;
                check(same_contract(m.contract, b.decoded.contract), "full contract equality");
                check(validate_view(b.decoded.frame_view(), desktop_limits().frame) == Status::ok,
                      "decoded canonical view");
                for (std::size_t p = 0; p < count; ++p) {
                    std::uint64_t out = 0;
                    check(load_bits(std::get<NumericView>(b.decoded.frame_view().payload), {p, 0, 0}, dtype,
                                    out) == Status::ok &&
                              out == bits,
                          "all dtype/rank/endian bits");
                }
                check(std::equal(m.numeric.storage.begin(), m.numeric.storage.end(),
                                 b.decoded.numeric.storage.begin()),
                      "payload bit preservation");
                primitive_endpoints(dtype,width,v,m,b);
                Message copied = b.decoded;
                Message moved = std::move(copied);
                check(validate_view(moved.frame_view(), desktop_limits().frame) == Status::ok,
                      "copy/move rebased contract pointer");

}
void primitive_matrix_cases() {
    for (const auto dtype : {DType::Int8, DType::UInt8, DType::Int16, DType::UInt16, DType::Int32,
                             DType::UInt32, DType::Int64, DType::UInt64, DType::Float32, DType::Float64})
        for (const auto rank : {Rank::Scalar, Rank::Vector, Rank::Matrix})
            for (const auto encoding : {StorageEncoding::FixedLittleEndian, StorageEncoding::FixedBigEndian})
                primitive_matrix_case(dtype, rank, encoding);
}
void pcm24_case(StorageEncoding encoding, BitPlacement placement) {
            if ((encoding == StorageEncoding::PackedSigned24LE ||
                 encoding == StorageEncoding::PackedSigned24BE) &&
                placement == BitPlacement::MostSignificant)
                return;
            std::array<std::uint32_t, 1> ch{0};
            std::array<PlaneLayout, 1> ps{};
            alignas(8) std::array<std::byte, 16> p{};
            NumericContract c;
            c.dtype = DType::Int32;
            c.semantic = Semantic::PCM;
            c.shape.rank = Rank::Vector;
            c.shape.axes[0] = Axis::Sample;
            c.shape.extents[0] = {2, 2};
            c.channel_order = ch;
            c.clock.domain = "pcm";
            c.clock.sample_rate = 48000;
            c.representation.encoding = encoding;
            c.representation.valid_bits = 24;
            c.representation.placement = placement;
            std::size_t w = 0;
            check(storage_width(c, w) == Status::ok, "PCM storage width");
            ps[0].stride_bytes[0] = w;
            auto m = base(c, std::span(p).first(2 * w), ps);
            m.numeric.actual = {2, 1};
            MutableNumericView v{&c, std::span(p).first(2 * w), ps, {2, 1}, {}, {}};
            check(store_element(v, {0, 0, 0}, std::int32_t{-8388608}) == Status::ok, "PCM24 min");
            check(store_element(v, {0, 1, 0}, std::int32_t{8388607}) == Status::ok, "PCM24 max");
            if (encoding == StorageEncoding::PackedSigned24LE)
                check(p[0] == std::byte{0} && p[1] == std::byte{0} && p[2] == std::byte{128},
                      "literal packed24 LE golden");
            if (encoding == StorageEncoding::PackedSigned24BE)
                check(p[0] == std::byte{128} && p[1] == std::byte{0} && p[2] == std::byte{0},
                      "literal packed24 BE golden");
            Buffers b;
            if (b.roundtrip(m)) {
                std::int32_t x = 0;
                check(load_element(std::get<NumericView>(b.decoded.frame_view().payload), {}, x) ==
                              Status::ok &&
                          x == -8388608,
                      "PCM decode logical Int32");
            }
            if (w == 4) {
                const bool be = encoding == StorageEncoding::FixedBigEndian;
                const auto padding =
                    placement == BitPlacement::LeastSignificant ? (be ? 0U : 3U) : (be ? 3U : 0U);
                p[padding] = placement == BitPlacement::LeastSignificant ? std::byte{0} : std::byte{1};
                check(!measure(m, desktop_limits()), "noncanonical valid24 padding refused");
            }
            m.numeric.storage = m.numeric.storage.first(2 * w - 1);
            check(!measure(m, desktop_limits()), "truncated PCM sample refused");

}
void pcm24_cases() {
    for (auto encoding : {StorageEncoding::PackedSigned24LE, StorageEncoding::PackedSigned24BE,
                          StorageEncoding::FixedLittleEndian, StorageEncoding::FixedBigEndian})
        for (auto placement : {BitPlacement::LeastSignificant, BitPlacement::MostSignificant})
            pcm24_case(encoding, placement);
}
void layout_coordinate_case(bool inter) {
        std::array<std::uint32_t, 3> ch{10, 11, 12};
        std::array<PlaneLayout, 3> ps{};
        alignas(8) std::array<std::byte, 40> p{};
        std::array<std::byte, 16> coords{};
        NumericContract c;
        c.dtype = DType::UInt16;
        c.semantic = Semantic::PCM;
        c.shape.rank = Rank::Vector;
        c.shape.axes[0] = Axis::Sample;
        c.shape.extents[0] = {2, 2};
        c.channel_order = ch;
        c.clock.domain = "pcm";
        c.representation.encoding = StorageEncoding::FixedLittleEndian;
        c.representation.layout = inter ? ChannelLayout::Interleaved : ChannelLayout::Planar;
        c.coordinates[0].mode = CoordinateMode::ExplicitFloat64LE;
        for (std::size_t n = 0; n < 3; ++n)
            ps[n] = {inter ? n * 2 : n * 12, {inter ? 8U : 4U, 0}};
        auto m = base(c, p, ps);
        m.numeric.actual = {2, 1};
        m.numeric.coordinates.axes[0] = coords;
        Buffers b;
        check(bool(b.roundtrip(m)), "interleaved/padded/coordinates");
        for (unsigned i = 0; i < 8; ++i)
            coords[i] = std::byte((0x7ff8000000000000ULL >> (8 * i)) & 255);
        check(!measure(m, desktop_limits()), "nonfinite coordinate refused regardless of value domain");

}
void layout_coordinate_cases() {
    for (bool inter : {false, true}) layout_coordinate_case(inter);
}
void matrix_cases() {
    primitive_matrix_cases();
    pcm24_cases();
    layout_coordinate_cases();
}
void boundary_literal_golden(Buffers& b,const auto& e){
    const std::array<std::byte, 12> prefix{std::byte{0x4e}, std::byte{0x44}, std::byte{0x41}, std::byte{0x54},
                                           std::byte{6},    std::byte{0},    std::byte{1},    std::byte{0},
                                           std::byte{1},    std::byte{0},    std::byte{1},    std::byte{0}};
    check(std::equal(prefix.begin(), prefix.end(), b.wire.begin()), "literal fixed header golden");
    // Independent DATA6 field table: DATA5 fields plus nonlinear-latency flag
    // at183, before the contract. Canonical zero latency is0/1. Complete450 bytes.
    std::array<std::byte, 450> golden{};
    std::copy(prefix.begin(), prefix.end(), golden.begin());
    for (auto [at, value] :
         {std::pair{12, 1}, {20, 2},  {28, 3},    {36, 4},  {44, 5},  {52, 6},   {72, 1},  {75, 4},
          {81, 7},          {137, 1}, {139, 'x'}, {140, 1}, {175, 1}, {183, 1}, {184, 10}, {186, 1}, {187, 8},
          {191, 1},         {199, 1}, {208, 1},   {216, 1}, {224, 1}, {226, 1},  {227, 1}, {229, 1},
          {247, 1},         {263, 1}, {281, 1},   {297, 1}, {305, 5}, {312, 1},  {320, 1}, {332, 1},
          {334, 7},         {362, 1}, {370, 1},   {395, 1}, {421, 8}})
        golden[at>=183?at+1:at] = std::byte(value);
    for (unsigned n = 0; n < 4; ++n)
        golden[77 + n] = std::byte("main"[n]);
    for (unsigned n = 0; n < 5; ++n)
        golden[308 + n] = std::byte("event"[n]);
    check(e.produced == golden.size() && std::equal(golden.begin(), golden.end(), b.wire.begin()),
          "complete literal scalar wire golden");
    check(bool(decode(golden, b.decoded, {b.bytes, b.text, b.channels, b.planes}, desktop_limits())),
          "literal golden decode independent of encode");
    std::array<std::byte, 451> misaligned_wire{};
    std::copy(golden.begin(), golden.end(), misaligned_wire.begin() + 1);
    check(bool(decode(std::span(misaligned_wire).subspan(1), b.decoded,
                      {b.bytes, b.text, b.channels, b.planes}, desktop_limits())),
          "wire input may be unaligned safely");
    b.bytes.fill(std::byte{12});
    auto invalid_arena = Arena{std::span(b.bytes).subspan(1), b.text, b.channels, b.planes};
    auto alignment_error = decode(golden, b.decoded, invalid_arena, desktop_limits());
    check(!alignment_error && !alignment_error.consumed && b.bytes[1] == std::byte{12},
          "misaligned destination arena refusal atomic");
}
void boundary_corruption(Buffers& b,const auto& e){
    for (std::size_t n = 0; n < e.produced; ++n) {
        b.bytes.fill(std::byte{0x55});
        b.text.fill('z');
        b.channels.fill(42);
        b.planes.fill({77, {88, 99}});
        auto before = b.decoded;
        auto r = decode(std::span(b.wire).first(n), b.decoded, {b.bytes, b.text, b.channels, b.planes},
                        desktop_limits());
        check(!r && !r.consumed && b.decoded.envelope.session == before.envelope.session,
              "truncation refuses atomically");
        check(b.bytes[0] == std::byte{0x55} && b.text[0] == 'z' && b.channels[0] == 42 &&
                  b.planes[0].offset == 77,
              "all arenas unchanged");
    }
    for (auto at : {0U, 4U, 6U, 8U, 9U, 10U, 11U}) {
        auto old = b.wire[at];
        b.wire[at] = std::byte{255};
        check(!inspect(std::span(b.wire).first(e.produced), desktop_limits()),
              "magic/version/grammar/kind/reserved/value refusal");
        b.wire[at] = old;
    }
    {const auto old=b.wire[183];b.wire[183]=std::byte{255};
        check(!inspect(std::span(b.wire).first(e.produced),desktop_limits()),"nonlinear latency boolean refused");b.wire[183]=old;}
    for (auto at : {183U, 184U, 185U, 186U, 187U, 188U, 189U, 190U, 207U, 224U, 226U, 227U, 228U, 237U, 238U,
                    271U, 272U, 394U}) {
        ++at; // DATA6 inserts one boolean before the descriptor.
        auto old = b.wire[at];
        b.wire[at] = std::byte{255};
        check(!inspect(std::span(b.wire).first(e.produced), desktop_limits()),
              "each descriptor symbol/boolean refused");
        b.wire[at] = old;
    }
}
void boundary_cases() {
    std::array<std::uint32_t, 1> ch{7};
    std::array<PlaneLayout, 1> ps{};
    alignas(8) std::array<std::byte, 8> payload{};
    NumericContract c;
    c.dtype = DType::Float64;
    c.channel_order = ch;
    c.clock.domain = "event";
    c.representation.encoding = StorageEncoding::FixedLittleEndian;
    auto m = base(c, payload, ps);
    Buffers b;
    auto e = encode(m, b.wire, b.scratch, desktop_limits());
    check(bool(e), "boundary seed");
    if (!e)
        return;
    boundary_literal_golden(b,e);
    boundary_corruption(b,e);
    boundary_capacities(m,b,e);
    boundary_extensions(m,b);
    boundary_ieee_values(m,b,payload);
    boundary_retained_ownership(m,b,ch);
    boundary_structured_negotiation(m,b);
}
void scalar_history_cases() {
    std::array<std::uint32_t,1> channels{7};
    std::array<PlaneLayout,1> planes{{{0,{0,0}}}};
    std::array<std::byte,4> payload{};
    NumericContract history;
    history.dtype=DType::Float32;
    history.semantic=Semantic::History;
    history.history_element=Semantic::Scalar;
    history.channel_order=channels;
    history.shape={Rank::Vector,{Axis::Time,Axis::None},{ExtentBounds{4,4},ExtentBounds{}}};
    history.representation.encoding=StorageEncoding::FixedLittleEndian;
    history.clock.domain="scalar-history";
    auto scalar=history;
    scalar.semantic=Semantic::Scalar;
    scalar.history_element=Semantic::Numeric;
    scalar.shape={Rank::Scalar,{Axis::None,Axis::None},{ExtentBounds{},ExtentBounds{}}};
    auto message=base(scalar,payload,planes);
    message.kind=Kind::HistoryRow;
    message.numeric.actual={1,1};
    message.history_contract=history;
    message.history={3,4,5};
    Buffers buffers;
    check(bool(buffers.roundtrip(message)) &&
          std::get<NumericContract>(buffers.decoded.contract).shape.rank==Rank::Scalar &&
          buffers.decoded.history.capacity==4,"scalar History compact DATA roundtrip");
    message.reference={5,6};
    Binding binding{message.reference,message.endpoint,message.stamp.stream_id,history,history,message.values};
    ReferenceContext context{1,2,6,std::span(&binding,1)};
    const auto encoded=encode(message,buffers.wire,buffers.scratch,desktop_limits(),context);
    check(bool(encoded),"registered vector History scalar row encode");
    if(encoded){
        const auto decoded=decode(std::span(buffers.wire).first(encoded.produced),buffers.decoded,
            {buffers.bytes,buffers.text,buffers.channels,buffers.planes},desktop_limits(),context);
        check(bool(decoded) && same_contract(buffers.decoded.contract,DataContract{scalar}),
            "registered vector History scalar row decode");
    }
    message.numeric.storage={};
    message.reference={};
    check(!measure(message,desktop_limits()),"scalar History row refuses absent payload");
}
void history_cases() {
    scalar_history_cases();
    std::array<std::uint32_t, 2> ch{1, 2};
    std::array<PlaneLayout, 2> rowplanes{{{0, {2, 0}}, {6, {2, 0}}}};
    alignas(8) std::array<std::byte, 36> ring{};
    NumericContract h;
    h.dtype = DType::Int16;
    h.semantic = Semantic::History;
    h.history_element = Semantic::MelEnergy;
    h.channel_order = ch;
    h.shape.rank = Rank::Matrix;
    h.shape.axes = {Axis::Time, Axis::Band};
    h.shape.extents = {ExtentBounds{0, 3}, ExtentBounds{3, 3}};
    h.representation.encoding = StorageEncoding::FixedLittleEndian;
    h.clock.domain = "rows";
    auto row = h;
    row.semantic = Semantic::MelEnergy;
    row.history_element = Semantic::Numeric;
    row.shape.rank = Rank::Vector;
    row.shape.axes = {Axis::Band, Axis::None};
    row.shape.extents = {ExtentBounds{3, 3}, ExtentBounds{}};
    Message m;Buffers b;compact_history_row_seed(m,b,h,row,ring,rowplanes);
    std::array<PlaneLayout, 2> histplanes{{{0, {6, 2}}, {18, {6, 2}}}};
    m.numeric.planes = histplanes;
    m.numeric.actual = {3, 3};
    m.numeric.ring = {3, 2, {}};
    m.history = {2, 3, 7};
    std::array<std::byte, 3> mask{std::byte{0}, std::byte{1}, std::byte{0}};
    m.numeric.ring.silence_mask = mask;
    check(!measure(m, desktop_limits()), "History refused ordinary callback record");
    m.kind = Kind::Snapshot;
    check(!measure(m,desktop_limits()),"snapshot missing query request refuses");
    m.envelope.request=99;
    check(bool(b.roundtrip(m)), "explicit wrapped snapshot");
    std::get<NumericContract>(m.contract).semantic=Semantic::PCM;
    check(bool(b.roundtrip(m)), "explicit ring snapshot independent of descriptive tag");
    std::get<NumericContract>(m.contract).semantic=Semantic::History;
    check(!measure(m, device_block_limits()), "device History OFF");
    check(b.decoded.numeric.ring.oldest == 2 && b.decoded.numeric.ring.silence_mask[1] == std::byte{1},
          "snapshot ring/mask metadata");
    mask[1] = std::byte{0};
    check(b.decoded.numeric.ring.silence_mask[1] == std::byte{1}, "snapshot mask owns copied bytes");
    m.numeric.ring.oldest = 0;
    check(bool(b.roundtrip(m)), "plain History snapshot remains accepted");
    history_record_markers(m,b);
    history_reference_row(m,b,row,h,ring,rowplanes);
}

void reference_cases() {
    std::array<std::uint32_t, 2> channels{0, 1};
    std::array<PlaneLayout, 2> planes{{{0, {4, 0}}, {1024, {4, 0}}}};
    alignas(8) std::array<std::byte, 2048> payload{};
    NumericContract c;
    c.dtype = DType::Float32;
    c.semantic = Semantic::PCM;
    c.shape.rank = Rank::Vector;
    c.shape.axes[0] = Axis::Sample;
    c.shape.extents[0] = {256, 256};
    c.channel_order = channels;
    c.representation.encoding = StorageEncoding::FixedLittleEndian;
    c.clock.domain = "capture";
    c.clock.sample_rate = 48000;
    auto m = base(c, payload, planes);
    m.numeric.actual = {256, 1};
    m.reference = {9, 11};
    Buffers b;
    Binding binding{m.reference, m.endpoint, m.stamp.stream_id, m.contract, {}, m.values};
    ReferenceContext context{1, 2, 6, std::span(&binding, 1)};
    auto long_identity=m;long_identity.endpoint.graph="012345678901234567890123456789012";
    check(validate(long_identity,device_block_limits())!=Status::ok,"device identity32 bound separate from schema64");
    auto e = encode(m, b.wire, b.scratch, device_block_limits(), context);
    check(bool(e) && e.produced == 2277, "device stereo256 compact reference required");
    if (!e)
        return;
    auto exact_limit=device_block_limits();exact_limit.message_bytes=2277;
    check(bool(measure(m,exact_limit,context)),"DATA5 compact reference exact envelope boundary");
    exact_limit.message_bytes=2276;
    check(!measure(m,exact_limit,context),"DATA5 compact reference one byte short rejects");
    reference_literal_golden(m,b,context,e);
    reference_context_refusals(m,b,binding,context,planes,e);
    auto d = decode(std::span(b.wire).first(e.produced), b.decoded, {b.bytes, b.text, b.channels, b.planes},
               device_block_limits(), context);
    check(bool(d), "reference donor lifetime seed");
    m.stamp.has_sample_interval=true;m.stamp.sample_begin=9007199254740993LL;m.stamp.sample_count=256;m.stamp.latency_samples=63;m.stamp.latency_seconds={1,700};
    m.stamp.frequency_dependent_latency=true;
    e=encode(m,b.wire,b.scratch,device_block_limits(),context);check(bool(e),"DATA6 reference timing encode");
    d=decode(std::span(b.wire).first(e.produced),b.decoded,{b.bytes,b.text,b.channels,b.planes},device_block_limits(),context);
    check(d&&b.decoded.stamp.has_sample_interval&&b.decoded.stamp.sample_begin==9007199254740993LL&&b.decoded.stamp.sample_count==256&&b.decoded.stamp.latency_samples==63&&b.decoded.stamp.latency_seconds.numerator==1&&b.decoded.stamp.latency_seconds.denominator==700&&b.decoded.stamp.frequency_dependent_latency,"referenced DATA6 exact sample interval and latency model");
    binding.endpoint.graph = "gone";
    binding.stream = "gone";
    std::get<NumericContract>(binding.contract).clock.domain = "gone";
    channels[0] = 999;
    b.wire.fill(std::byte{255});
    check(b.decoded.endpoint.graph == "main" && b.decoded.stamp.stream_id == "x" &&
              std::get<NumericContract>(b.decoded.contract).clock.domain == "capture" &&
              std::get<NumericContract>(b.decoded.contract).channel_order[0] == 0,
          "reference table lifetime independent");
}



void snapshot_contract_cases() {
    std::array<std::uint32_t, 1> channels{0};
    std::array<PlaneLayout, 1> planes{};
    alignas(8) std::array<std::byte, 1> payload{};
    for (auto rank : {Rank::Scalar, Rank::Vector, Rank::Matrix}) {
        NumericContract contract;
        contract.dtype = DType::UInt8;
        contract.channel_order = channels;
        contract.representation.encoding = StorageEncoding::FixedLittleEndian;
        contract.shape.rank = rank;
        contract.semantic = rank == Rank::Scalar ? Semantic::Scalar : Semantic::Numeric;
        planes[0] = {};
        if (rank != Rank::Scalar) {
            contract.shape.axes[0] = Axis::Declared;
            planes[0].stride_bytes[0] = 1;
        }
        if (rank == Rank::Matrix) {
            contract.shape.axes[1] = Axis::Declared;
            planes[0].stride_bytes[1] = 1;
        }
        auto frame = base(contract, payload, planes);
        frame.kind = Kind::Snapshot;
        frame.envelope.request = 55;
        Buffers buffers;
        buffers.wire.fill(std::byte{0x7a});
        check(validate(frame, desktop_limits()) != Status::ok,
              "Snapshot requires explicit ring metadata on public validator");
        check(!measure(frame, desktop_limits()), "Snapshot refuses scalar/vector or Matrix without ring metadata");
        auto encoded = encode(frame, buffers.wire, buffers.scratch, desktop_limits());
        check(!encoded && !encoded.produced && buffers.wire[0] == std::byte{0x7a},
              "non-ring Snapshot encode atomic refusal");
        frame.reference = {1, 2};
        Binding binding{frame.reference, frame.endpoint, frame.stamp.stream_id, frame.contract, {},
                        frame.values};
        ReferenceContext context{1, 2, 6, std::span(&binding, 1)};
        check(!measure(frame, desktop_limits(), context), "non-ring reference Snapshot refused");
    }
    auto structured = base(NumericContract{}, payload, {});
    structured.contract = StructuredContract{"custom", 1, "opaque", {1, 1}};
    structured.numeric = {};
    structured.structured = payload;
    structured.kind = Kind::Snapshot;
    structured.envelope.request = 55;
    check(!measure(structured, desktop_limits()), "Snapshot refuses Structured contract");
}
