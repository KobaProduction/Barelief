#include "text_validation.h"
#include <bdsp_protocol/version_admission.h>
#include <bdsp_protocol/numeric_control_codec.h>
#include <bit>
#include <cstring>
#include <limits>
#include <iterator>
#include <new>
#include <cmath>
namespace bdsp_protocol::numeric_control {
std::optional<CodecId> negotiate(std::uint32_t a, std::uint32_t b, std::uint32_t x, std::uint32_t y) noexcept {
    if (a != SchemaVersion || b != SchemaVersion || ((x | y) & ~3U))
        return {};
    if (x & y & CborMask)
        return CodecId::Cbor;
    if (x & y & ProtobufMask)
        return CodecId::Protobuf;
    return {};
}
namespace detail {
bool Context::fail(Status s) noexcept {
    if (error == Status::Ok)
        error = s;
    return false;
}
void *Context::reserve(std::size_t n, std::size_t width, std::size_t alignment) noexcept {
    if (n > limits.records - records || n > SIZE_MAX / width) {
        fail(Status::Limit);
        return nullptr;
    }
    records += n;
    auto padding = (alignment - used % alignment) % alignment;
    if (used > SIZE_MAX - padding || n * width > SIZE_MAX - used - padding) {
        fail(Status::Overflow);
        return nullptr;
    }
    auto start = used + padding;
    used = start + n * width;
    if (used > limits.arena_bytes) {
        fail(Status::Limit);
        return nullptr;
    }
    return materializing && !arena.empty() ? arena.data() + start : nullptr;
}
bool utf8(std::string_view text) noexcept {
    return bdsp_protocol::text_detail::utf8_without_nul(text);
}

bool Context::store(std::string_view &s, std::size_t maximum, bool binary) noexcept {
    if (s.size() > maximum || s.size() > limits.total_text_bytes - text)
        return fail(Status::Limit);
    if (!binary && !utf8(s))
        return fail();
    if (binary) {
        if (s.size() > limits.extension_bytes - binary_bytes)
            return fail(Status::Limit);
        binary_bytes += s.size();
    }
    text += s.size();
    return (!reading && !validating) || store_text_bytes(s);
}
bool Context::store_text_bytes(std::string_view &s) noexcept {
    // Text bytes are not descriptor records.
    if (s.empty()) {
        if (materializing) s = {};
        return true;
    }
    if (s.size() > limits.arena_bytes - used) return fail(Status::Limit);
    auto *p = materializing ? arena.data() + used : nullptr;
    used += s.size();
    if (materializing) {
        std::memcpy(p, s.data(), s.size());
        s = {reinterpret_cast<const char *>(p), s.size()};
    }
    return true;
}
unsigned max_tag(Schema s) noexcept {
    constexpr unsigned tags[]{
        5, 2, 2, 4, 6, 17, 5, 2, 10, 2, 5, 21, 7, 2, 3,
        6, 5, 5, 6, 8, 5, 5, 5, 5, 6, 5, 2, 9, 0
    };
    const auto index = static_cast<unsigned>(s);
    return index < std::size(tags) ? tags[index] : 0;
}
namespace {
using namespace signal_contracts;
template <class T> bool u(Archive &a, unsigned tag, T &v) noexcept {
    std::uint64_t n = v;
    if (!a.uint_field(tag, n))
        return false;
    if (n > std::numeric_limits<T>::max())
        return a.state.fail();
    v = static_cast<T>(n);
    return true;
}
template <class T> bool si(Archive &a, unsigned tag, T &v) noexcept {
    std::int64_t n = v;
    if (!a.int_field(tag, n))
        return false;
    if (n < std::numeric_limits<T>::min() || n > std::numeric_limits<T>::max())
        return a.state.fail();
    v = static_cast<T>(n);
    return true;
}
template <class T> bool sym(Archive &a, unsigned tag, T &v) noexcept {
    std::uint8_t id = 0;
    if (!a.state.reading && !numeric_data::symbol_id(v, id))
        return a.state.fail();
    if (!u(a, tag, id))
        return false;
    return numeric_data::symbol_value(id, v) || a.state.fail();
}
template <class T, std::size_t N> bool choice(Archive &a, unsigned tag, T &v, const T (&values)[N]) noexcept {
    std::uint64_t id = 0;
    if (!a.state.reading)
        for (std::size_t i = 0; i < N; ++i)
            if (values[i] == v)
                id = i + 1;
    if (!a.uint_field(tag, id) || !id || id > N)
        return a.state.fail();
    v = values[id - 1];
    return true;
}
template <class T> bool optional(Archive &a, unsigned tag, Schema schema, std::optional<T> &v, Visit f) noexcept {
    bool present = v.has_value();
    T item = v.value_or(T{});
    if (!a.object(tag, schema, &item, f, &present))
        return false;
    if (a.state.reading) {
        if (present)
            v = item;
        else
            v.reset();
    }
    return true;
}
template <class T> struct Seq {
    std::span<const T> source;
    T *output;
    Visit visit;
};
template <class T> bool element(Archive &a, void *p, std::size_t i) noexcept {
    auto &s = *static_cast<Seq<T> *>(p);
    T v = a.state.reading ? T{} : s.source[i];
    if (!s.visit(a, &v))
        return false;
    if (s.output)
        new (s.output + i) T(v);
    return true;
}
template <class T>
T *sequence_storage(Context &state, std::size_t count) noexcept {
    if (state.reading || state.validating)
        return static_cast<T *>(state.reserve(count, sizeof(T), alignof(T)));
    return nullptr;
}
template <class T>
bool seq(Archive &a, unsigned tag, Schema schema, std::span<const T> &v, std::size_t maximum, Visit f) noexcept {
    std::size_t n = v.size();
    if (!a.sequence_size(tag, n))
        return false;
    if (n > maximum)
        return a.state.fail(Status::Limit);
    T *out = sequence_storage<T>(a.state, n);
    if (a.state.error != Status::Ok)
        return false;
    Seq<T> s{v, out, f};
    if (!a.sequence(tag, schema, &s, n, element<T>))
        return false;
    if (a.state.reading && a.state.materializing)
        v = {out, n};
    return true;
}
bool literal_signed(Archive &a, Literal &v) noexcept {
    std::int64_t n = a.state.reading ? 0 : std::get<std::int64_t>(v);
    if (!a.int_field(2, n)) return false;
    v = n;
    return true;
}
bool literal_unsigned(Archive &a, Literal &v) noexcept {
    std::uint64_t n = a.state.reading ? 0 : std::get<std::uint64_t>(v);
    if (!a.uint_field(3, n)) return false;
    v = n;
    return true;
}
bool literal_float(Archive &a, Literal &v) noexcept {
    std::uint32_t n = a.state.reading ? 0 : std::bit_cast<std::uint32_t>(std::get<float>(v));
    if (!u(a, 4, n)) return false;
    v = std::bit_cast<float>(n);
    return true;
}
bool literal_double(Archive &a, Literal &v) noexcept {
    std::uint64_t n = a.state.reading ? 0 : std::bit_cast<std::uint64_t>(std::get<double>(v));
    if (!a.uint_field(5, n)) return false;
    v = std::bit_cast<double>(n);
    return true;
}
bool literal(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Literal *>(p);
    std::uint64_t tag = a.state.reading ? 0 : v.index() + 1;
    if (!a.uint_field(1, tag)) return false;
    switch (tag) {
    case 1: return literal_signed(a, v);
    case 2: return literal_unsigned(a, v);
    case 3: return literal_float(a, v);
    case 4: return literal_double(a, v);
    default: return a.state.fail();
    }
}
bool ratio(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Ratio *>(p);
    return a.int_field(1, v.numerator) && a.uint_field(2, v.denominator) && (v.denominator != 0 || a.state.fail());
}
bool extent(Archive &a, void *p) noexcept {
    auto &v = *static_cast<ExtentBounds *>(p);
    return a.uint_field(1, v.minimum) && a.uint_field(2, v.maximum) && (v.minimum <= v.maximum || a.state.fail());
}
bool coordinates(Archive &a, void *p) noexcept {
    auto &v = *static_cast<AxisCoordinates *>(p);
    return sym(a, 1, v.mode) && sym(a, 2, v.unit) && a.object(3, Schema::Ratio, &v.origin, ratio) &&
           a.object(4, Schema::Ratio, &v.step, ratio);
}
bool descriptor_encoding(Archive &a, unsigned tag, StorageEncoding &value) noexcept {
    std::uint8_t id = NativeDescriptorEncoding;
    if (!a.state.reading && value != StorageEncoding::Native && !numeric_data::symbol_id(value, id))
        return a.state.fail();
    if (!u(a, tag, id))
        return false;
    if (id == NativeDescriptorEncoding) {
        value = StorageEncoding::Native;
        return true;
    }
    return numeric_data::symbol_value(id, value) || a.state.fail();
}
bool representation(Archive &a, void *p) noexcept {
    auto &v = *static_cast<RepresentationContract *>(p);
    return descriptor_encoding(a, 1, v.encoding) && u(a, 2, v.valid_bits) && sym(a, 3, v.placement) &&
           sym(a, 4, v.layout) && sym(a, 5, v.normalization) && u(a, 6, v.alignment);
}
bool uint_element(Archive &a, void *p) noexcept {
    return u(a, 0, *static_cast<std::uint32_t *>(p));
}
bool numeric(Archive &a, void *p) noexcept {
    auto &v = *static_cast<NumericContract *>(p);
    return sym(a, 1, v.dtype) && sym(a, 2, v.shape.rank) && sym(a, 3, v.shape.axes[0]) && sym(a, 4, v.shape.axes[1]) &&
           a.object(5, Schema::Extent, &v.shape.extents[0], extent) &&
           a.object(6, Schema::Extent, &v.shape.extents[1], extent) && sym(a, 7, v.semantic) &&
           sym(a, 8, v.history_element) && sym(a, 9, v.quantity) && sym(a, 10, v.unit) &&
           seq(a, 11, Schema::UIntElement, v.channel_order, a.state.limits.channels, uint_element) &&
           a.object(12, Schema::Representation, &v.representation, representation) &&
           a.object(13, Schema::Coordinates, &v.coordinates[0], coordinates) &&
           a.object(14, Schema::Coordinates, &v.coordinates[1], coordinates) && a.text_field(15, v.clock.domain) &&
           a.object(16, Schema::Ratio, &v.clock.tick_seconds, ratio) && u(a, 17, v.clock.sample_rate);
}
bool structured(Archive &a, void *p) noexcept {
    auto &v = *static_cast<StructuredContract *>(p);
    return a.text_field(1, v.schema) && u(a, 2, v.version) && a.text_field(3, v.encoding) &&
           a.uint_field(4, v.payload_bytes.minimum) && a.uint_field(5, v.payload_bytes.maximum);
}
template <class T>
bool contract_alternative(Archive &a, DataContract &v, unsigned tag, Schema schema, Visit visitor,
                          unsigned &selected) noexcept {
    T item{};
    bool present = !a.state.reading && std::holds_alternative<T>(v);
    if (present) item = std::get<T>(v);
    if (!a.object(tag, schema, &item, visitor, &present)) return false;
    if (present) {
        v = item;
        ++selected;
    }
    return true;
}
bool contract(Archive &a, void *p) noexcept {
    auto &v = *static_cast<DataContract *>(p);
    unsigned selected = 0;
    if (!contract_alternative<NumericContract>(a, v, 1, Schema::Numeric, numeric, selected) ||
        !contract_alternative<StructuredContract>(a, v, 2, Schema::Structured, structured, selected)) return false;
    if (selected != 1)
        return a.state.fail();
    if (!a.state.reading || a.state.materializing)
        return validate_contract(v, a.state.limits.frame) == signal_contracts::Status::ok || a.state.fail();
    return true;
}
bool dyadic(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Dyadic *>(p);
    return a.object(1, Schema::Literal, &v.numerator, literal) && si(a, 2, v.exponent2);
}
bool policy(Archive &a, void *p) noexcept {
    auto &v = *static_cast<ConversionPolicy *>(p);
    constexpr Rounding rounds[]{Rounding::TowardZero, Rounding::NearestTiesEven};
    constexpr Overflow over[]{Overflow::Reject, Overflow::Saturate};
    constexpr PrecisionLoss prec[]{PrecisionLoss::RejectInexact, PrecisionLoss::AllowDeclared};
    constexpr NonFinite nf[]{NonFinite::Reject, NonFinite::ReplaceFinite, NonFinite::ClampFinite};
    constexpr ScaleMode scales[]{ScaleMode::Identity,     ScaleMode::IntegerAffine,  ScaleMode::DyadicAffine,
                                 ScaleMode::PcmNormalize, ScaleMode::PcmDenormalize, ScaleMode::Binary64Affine};
    return choice(a, 1, v.rounding, rounds) && choice(a, 2, v.overflow, over) && choice(a, 3, v.precision_loss, prec) &&
           choice(a, 4, v.nonfinite, nf) && choice(a, 5, v.scaling.mode, scales) &&
           a.object(6, Schema::Dyadic, &v.scaling.gain, dyadic) &&
           a.object(7, Schema::Dyadic, &v.scaling.offset, dyadic) &&
           a.object(8, Schema::Literal, &v.nan_replacement, literal) &&
           a.object(9, Schema::Literal, &v.negative_inf_replacement, literal) &&
           a.object(10, Schema::Literal, &v.positive_inf_replacement, literal);
}
bool parameter_literal(Archive &a, Parameter &v) noexcept {
    Literal value = a.state.reading ? Literal{std::int64_t{0}} : std::get<Literal>(v.value);
    if (!a.object(3, Schema::Literal, &value, literal)) return false;
    v.value = value;
    return true;
}
bool parameter_bool(Archive &a, Parameter &v) noexcept {
    bool value = a.state.reading ? false : std::get<bool>(v.value);
    if (!a.bool_field(4, value)) return false;
    v.value = value;
    return true;
}
bool parameter_text(Archive &a, Parameter &v) noexcept {
    std::string_view value = a.state.reading ? std::string_view{} : std::get<std::string_view>(v.value);
    if (!a.text_field(5, value)) return false;
    v.value = value;
    return true;
}
bool parameter(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Parameter *>(p);
    if (!a.text_field(1, v.key)) return false;
    std::uint64_t kind = a.state.reading ? 0 : v.value.index()+1;
    if (!a.uint_field(2, kind)) return false;
    bool valid = false;
    switch (kind) {
    case 1: valid = parameter_literal(a, v); break;
    case 2: valid = parameter_bool(a, v); break;
    case 3: valid = parameter_text(a, v); break;
    default: return a.state.fail();
    }
    return valid && (!v.key.empty() || a.state.fail());
}
bool binary64(Archive& a,unsigned tag,double& value) noexcept {
    auto bits=std::bit_cast<std::uint64_t>(value);
    if(!a.uint_field(tag,bits))return false;
    value=std::bit_cast<double>(bits);return std::isfinite(value)||a.state.fail();
}
bool oscillator(Archive& a,void* p) noexcept {
    auto& v=*static_cast<signal_contracts::OscillatorParameters*>(p);
    return a.uint_field(1,v.id)&&a.bool_field(2,v.enabled)&&binary64(a,3,v.frequency_hz)&&binary64(a,4,v.amplitude)&&
        binary64(a,5,v.phase_radians)&&binary64(a,6,v.attack_seconds)&&binary64(a,7,v.release_seconds)&&
        ((v.id && v.frequency_hz>0 && v.attack_seconds>=0 && v.release_seconds>=0) || a.state.fail());
}
bool mixer_weight(Archive& a,void* p) noexcept {
    auto& v=*static_cast<domain::MixerInputWeight*>(p);
    return u(a,1,v.port)&&binary64(a,2,v.weight);
}
std::uint64_t primary_parameter_kind(const Parameters& value) noexcept {
    if(std::holds_alternative<EndpointParameters>(value))return 1;
    if(std::holds_alternative<TransformParameters>(value))return 2;
    if(std::holds_alternative<domain::ConverterProcessingParameters>(value))return 3;
    if(std::holds_alternative<domain::RouteProcessingParameters>(value))return 4;
    if(std::holds_alternative<ReductionParameters>(value))return 5;
    return 0;
}

std::uint64_t parameter_kind(const Parameters& value) noexcept {
    if(const auto primary=primary_parameter_kind(value))return primary;
    if(std::holds_alternative<domain::HistoryProcessingParameters>(value))return 6;
    if(std::holds_alternative<GeneratorParameters>(value))return 7;
    if(std::holds_alternative<ClockParameters>(value))return 8;
    if(std::holds_alternative<MixerParameters>(value))return 9;
    return 0;
}
bool unique_parameter_keys(std::span<const Parameter> values) noexcept {
    for (std::size_t i = 0; i < values.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (values[i].key == values[j].key) return false;
    return true;
}
bool endpoint_parameters(Archive &a, Parameters &v, std::uint64_t kind) noexcept {
    std::span<const Parameter> values;
    if (!a.state.reading)
        values = kind == 1 ? std::get<EndpointParameters>(v).values : std::get<TransformParameters>(v).values;
    if (!seq(a, 2, Schema::Parameter, values, a.state.limits.parameters, parameter)) return false;
    if ((!a.state.reading || a.state.materializing) && !unique_parameter_keys(values)) return a.state.fail();
    if (kind == 1) v = EndpointParameters{values};
    else v = TransformParameters{values};
    return true;
}
bool converter_parameters(Archive &a, Parameters &v) noexcept {
    domain::ConverterProcessingParameters c;
    if (!a.state.reading) c = std::get<domain::ConverterProcessingParameters>(v);
    if (!a.object(3, Schema::Policy, &c.policy, policy)) return false;
    v = c;
    return true;
}
bool route_parameters(Archive &a, Parameters &v) noexcept {
    domain::RouteProcessingParameters r;
    if (!a.state.reading) r = std::get<domain::RouteProcessingParameters>(v);
    if (!u(a, 4, r.outputs) || !u(a, 5, r.selected) || !a.bool_field(6, r.open) || !u(a,21,r.channels_per_stream)) return false;
    v = r;
    return true;
}
bool reduction_parameters(Archive &a, Parameters &v) noexcept {
    ReductionParameters r;
    if (!a.state.reading) r = std::get<ReductionParameters>(v);
    constexpr domain::NumericReduction ops[]{domain::NumericReduction::MaxAbs, domain::NumericReduction::SignedMean, domain::NumericReduction::RMS};
    constexpr domain::ReductionChannels ch[]{domain::ReductionChannels::AllChannels, domain::ReductionChannels::BothChannels, domain::ReductionChannels::SelectedChannels};
    constexpr domain::ReductionRows rows[]{domain::ReductionRows::WholeValue, domain::ReductionRows::LatestHistoryRow};
    constexpr PrecisionLoss prec[]{PrecisionLoss::RejectInexact, PrecisionLoss::AllowDeclared};
    constexpr Rounding rounds[]{Rounding::TowardZero, Rounding::NearestTiesEven};
    if (!choice(a, 7, r.operation, ops) || !choice(a, 8, r.channels, ch) || !choice(a, 9, r.rows, rows) ||
        !seq(a, 10, Schema::UIntElement, r.selected_channels, a.state.limits.channels, uint_element) ||
        !choice(a, 11, r.precision_loss, prec) || !choice(a, 12, r.rounding, rounds)) return false;
    v = r;
    return true;
}
bool history_parameters(Archive &a, Parameters &v) noexcept {
    domain::HistoryProcessingParameters h;
    if (!a.state.reading) h = std::get<domain::HistoryProcessingParameters>(v);
    if (!a.uint_field(13, h.capacity)) return false;
    v = h;
    return true;
}
bool unique_oscillators(std::span<const signal_contracts::OscillatorParameters> values) noexcept {
    for (std::size_t i = 0; i < values.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (values[i].id == values[j].id) return false;
    return true;
}
bool generator_parameters(Archive &a, Parameters &v) noexcept {
    GeneratorParameters g;
    if (!a.state.reading) g = std::get<GeneratorParameters>(v);
    if (!seq(a, 14, Schema::Oscillator, g.oscillators,
             std::min<std::size_t>(signal_contracts::max_oscillators, a.state.limits.parameters), oscillator) ||
        ((!a.state.reading || a.state.materializing) && g.oscillators.empty())) return a.state.fail();
    if ((!a.state.reading || a.state.materializing) && !unique_oscillators(g.oscillators)) return a.state.fail();
    v = g;
    return true;
}
bool clock_parameters(Archive &a, Parameters &v) noexcept {
    ClockParameters c;
    if (!a.state.reading) c = std::get<ClockParameters>(v);
    if (!u(a, 15, c.sample_rate) || !a.text_field(16, c.domain) ||
        c.sample_rate < domain::min_sample_clock_rate || c.sample_rate > domain::max_sample_clock_rate || c.domain.empty()) return a.state.fail();
    v = c;
    return true;
}
bool valid_mixer_weights(const MixerParameters &m) noexcept {
    for (std::size_t i = 0; i < m.weights.size(); ++i) {
        if (domain::mixer_requires_nonnegative_weights(m.mode) && m.weights[i].weight < 0) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (m.weights[i].port == m.weights[j].port) return false;
    }
    return true;
}
bool mixer_parameters(Archive &a, Parameters &v) noexcept {
    MixerParameters m;
    if (!a.state.reading) m = std::get<MixerParameters>(v);
    constexpr domain::MixerMode modes[]{domain::MixerMode::WeightedSum, domain::MixerMode::WeightedMean, domain::MixerMode::WeightedRMS, domain::MixerMode::MaxAbsolute};
    constexpr domain::MixerConsumption consumption[]{domain::MixerConsumption::ClockedFifo, domain::MixerConsumption::SourceAligned};
    if (!choice(a, 17, m.mode, modes) || !seq(a, 18, Schema::MixerWeight, m.weights, a.state.limits.ports, mixer_weight) ||
        !u(a, 19, m.fifo_milliseconds) || !choice(a, 20, m.consumption, consumption) ||
        ((!a.state.reading || a.state.materializing) && m.weights.empty()) ||
        m.fifo_milliseconds < domain::min_mixer_fifo_ms || m.fifo_milliseconds > domain::max_mixer_fifo_ms) return a.state.fail();
    if ((!a.state.reading || a.state.materializing) && !valid_mixer_weights(m)) return a.state.fail();
    v = m;
    return true;
}
bool parameters(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Parameters *>(p);
    std::uint64_t kind = a.state.reading ? 0 : parameter_kind(v);
    if (!a.uint_field(1, kind)) return false;
    switch (kind) {
    case 1: case 2: return endpoint_parameters(a, v, kind);
    case 3: return converter_parameters(a, v);
    case 4: return route_parameters(a, v);
    case 5: return reduction_parameters(a, v);
    case 6: return history_parameters(a, v);
    case 7: return generator_parameters(a, v);
    case 8: return clock_parameters(a, v);
    case 9: return mixer_parameters(a, v);
    default: return a.state.fail();
    }
}
bool port(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Port *>(p);
    auto d =
        v.direction == domain::PortDirection::Input ? numeric_data::Direction::Input : numeric_data::Direction::Output;
    if (v.direction != domain::PortDirection::Input && v.direction != domain::PortDirection::Output)
        return a.state.fail();
    if (!u(a, 1, v.id) || !sym(a, 2, d) || !a.object(3, Schema::Contract, &v.contract, contract))
        return false;
    v.direction = d == numeric_data::Direction::Input ? domain::PortDirection::Input : domain::PortDirection::Output;
    return true;
}
bool node(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Node *>(p);
    return si(a, 1, v.id) && a.text_field(2, v.kind) && a.object(3, Schema::Parameters, &v.parameters, parameters) &&
           seq(a, 4, Schema::Port, v.inputs, a.state.limits.ports, port) &&
           seq(a, 5, Schema::Port, v.outputs, a.state.limits.ports, port) && a.bool_field(6, v.enabled);
}
bool edge(Archive &a, void *p) noexcept {
    auto &v = *static_cast<domain::Edge *>(p);
    return si(a, 1, v.from) && si(a, 2, v.to) && u(a, 3, v.output_port) && u(a, 4, v.input_port) && u(a,5,v.conversion) && (v.conversion<=3 || a.state.fail());
}
bool binding(Archive& a,void* p) noexcept {
    auto& v=*static_cast<domain::ConfigurationBinding*>(p);
    constexpr domain::ConfigurationRole roles[]{domain::ConfigurationRole::SampleClock,domain::ConfigurationRole::FrequencyGrid};
    std::uint8_t reserved=0;
    return si(a,1,v.from) && si(a,2,v.to) && choice(a,3,v.output,roles) && choice(a,4,v.input,roles) && u(a,5,reserved) && (reserved==0 || a.state.fail());
}
bool versions(Archive &a, void *p) noexcept {
    auto &v = *static_cast<GraphVersions *>(p);
    return u(a, 1, v.document) && u(a, 2, v.graph) && u(a, 3, v.processing) && u(a, 4, v.control) && u(a, 5, v.data) &&
           (graph_versions_supported(v) == signal_contracts::Status::ok ||
            a.state.fail(Status::UnsupportedVersion));
}
bool graph(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Graph *>(p);
    bool interval_present=true;
    return a.text_field(1, v.graph_id) && a.object(2, Schema::Versions, &v.versions, versions) &&
           seq(a, 3, Schema::Node, v.nodes, a.state.limits.nodes, node) &&
           seq(a, 4, Schema::Edge, v.edges, a.state.limits.edges, edge) &&
           a.object(5, Schema::Ratio, &v.execution_interval, ratio, &interval_present) &&
           seq(a,6,Schema::Edge,v.configuration_bindings,a.state.limits.edges,binding) &&
           (domain::valid_execution_interval(v.execution_interval) || a.state.fail());
}
constexpr CommandId commands[]{CommandId::Capabilities, CommandId::State,         CommandId::Prepare,
                               CommandId::Apply,        CommandId::SetParameters, CommandId::Start,
                               CommandId::Stop,         CommandId::Persist,       CommandId::Release};
bool command(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Command *>(p);
    if (!choice(a, 1, v.id, commands) || !optional(a, 2, Schema::Graph, v.graph, graph) ||
        !optional(a, 3, Schema::Parameters, v.parameters, parameters) || !a.text_field(4, v.graph_id) ||
        !a.uint_field(5, v.token) || !a.uint_field(6, v.graph_revision) || !si(a, 7, v.node) ||
        !a.bool_field(8, v.enabled))
        return false;
    return (v.id!=CommandId::Start && v.id!=CommandId::Stop &&
            v.graph.has_value() == (v.id == CommandId::Prepare) &&
            v.parameters.has_value() == (v.id == CommandId::SetParameters) &&
            (v.id == CommandId::Apply || v.graph_id.empty()) &&
            (v.id == CommandId::Apply || v.id == CommandId::Release || v.token == 0) &&
            (v.id == CommandId::Persist || v.graph_revision == 0) &&
            (v.id == CommandId::SetParameters || (v.node == -1 && v.enabled))) ||
           a.state.fail();
}
bool resources(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Resources *>(p);
    return a.uint_field(1, v.plan) && a.uint_field(2, v.payload) && a.uint_field(3, v.scratch) &&
           a.uint_field(4, v.stack) && a.uint_field(5, v.peak);
}
bool row(Archive &a, void *p) noexcept {
    auto &v = *static_cast<CapabilityRow *>(p);
    return a.object(1, Schema::Node, &v.combination, node) && a.text_field(2, v.kernel) && a.text_field(3, v.backend) &&
           sym(a, 4, v.values) && a.object(5, Schema::Resources, &v.resources, resources);
}
bool caps(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Capabilities *>(p);
    return u(a, 1, v.control_version) && u(a, 2, v.data_version) && u(a, 3, v.codec_mask) &&
           seq(a, 4, Schema::CapabilityRow, v.rows, a.state.limits.capability_rows, row) &&
           a.object(5, Schema::Resources, &v.maximum, resources) &&
           ((v.control_version == SchemaVersion && v.data_version == numeric_data::PayloadVersion && v.codec_mask != 0 && !(v.codec_mask & ~3U)) ||
            a.state.fail(Status::UnsupportedVersion));
}
template <class T> bool optu(Archive &a, unsigned tag, std::optional<T> &v) noexcept {
    bool present = v.has_value();
    std::uint64_t n = present ? static_cast<std::uint64_t>(*v) : 0;
    if (!a.optional_uint(tag, n, present))
        return false;
    if (present) {
        if (n > std::numeric_limits<T>::max())
            return a.state.fail();
        v = static_cast<T>(n);
    } else
        v.reset();
    return true;
}
bool diagnostic_reason(Archive& archive,domain::ProcessingReason& value) noexcept {
    std::optional<std::uint8_t> reason;
    if(value!=domain::ProcessingReason::None)reason=std::uint8_t(value);
    if(!optu(archive,5,reason))return false;
    if(reason && *reason>std::uint8_t(domain::ProcessingReason::ProfileEncoding))return archive.state.fail();
    value=reason?domain::ProcessingReason(*reason):domain::ProcessingReason::None;
    return true;
}

bool diagnostic(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Diagnostic *>(p);
    bool present = v.node.has_value();
    std::int64_t n = present ? *v.node : 0;
    if (!a.optional_int(1, n, present))
        return false;
    if (present) {
        if (n < INT32_MIN || n > INT32_MAX)
            return a.state.fail();
        v.node = static_cast<std::int32_t>(n);
    } else
        v.node.reset();
    return optu(a, 2, v.port) && optu(a, 3, v.edge) && a.text_field(4, v.parameter) && diagnostic_reason(a,v.reason);
}
bool state(Archive &a, void *p) noexcept {
    auto &v = *static_cast<State *>(p);
    constexpr Lifecycle lc[]{Lifecycle::Empty, Lifecycle::Prepared, Lifecycle::Stopped, Lifecycle::Running};
    return choice(a, 1, v.lifecycle, lc) && a.bool_field(2, v.run_intent) && a.uint_field(3, v.revision) &&
           a.uint_field(4, v.token) && optional(a, 5, Schema::Graph, v.applied, graph) &&
           a.object(6, Schema::Resources, &v.resources, resources);
}
bool reply(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Reply *>(p);
    constexpr Error errors[]{Error::None, Error::Conflict, Error::Validation, Error::Unsupported, Error::Limits,
                             Error::Busy, Error::Expired,  Error::Foreign,    Error::Internal};
    return choice(a, 1, v.command, commands) && choice(a, 2, v.error, errors) &&
           a.object(3, Schema::Diagnostic, &v.diagnostic, diagnostic) && a.object(4, Schema::State, &v.state, state) &&
           optional(a, 5, Schema::Capabilities, v.capabilities, caps);
}
bool extension(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Extension *>(p);
    std::string_view bytes{reinterpret_cast<const char *>(v.bytes.data()), v.bytes.size()};
    if (!u(a, 1, v.tag) || !a.text_field(2, bytes, true))
        return false;
    v.bytes = {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()};
    return (v.tag >= 1024 && v.tag <= 32767) || a.state.fail(Status::UnsupportedField);
}
} // namespace
template <class T>
bool visit_body_alternative(Archive &a, Message &v, unsigned tag, Schema schema, Visit visitor,
                            unsigned &selected) noexcept {
    T item{};
    bool present = !a.state.reading && std::holds_alternative<T>(v.body);
    if (present) item = std::get<T>(v.body);
    if (!a.object(tag, schema, &item, visitor, &present)) return false;
    if (present) {
        v.body = item;
        ++selected;
    }
    return true;
}
bool visit_body(Archive &a, Message &v) noexcept {
    unsigned selected = 0;
    if (!visit_body_alternative<Command>(a, v, 7, Schema::Command, command, selected) ||
        !visit_body_alternative<Reply>(a, v, 8, Schema::Reply, reply, selected)) return false;
    return selected == 1 || a.state.fail();
}
bool unique_extension_tags(std::span<const Extension> extensions) noexcept {
    for (std::size_t i = 0; i < extensions.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (extensions[i].tag == extensions[j].tag) return false;
    return true;
}
bool visit_message(Archive &a, void *p) noexcept {
    auto &v = *static_cast<Message *>(p);
    if (!u(a, 1, v.schema_version)) return false;
    if (v.schema_version != SchemaVersion ||
        bdsp_protocol::processing_boundary_version_supported(bdsp_protocol::ProcessingBoundary::Control, v.schema_version) !=
            signal_contracts::Status::ok) return a.state.fail(Status::UnsupportedVersion);
    if (!a.uint_field(2, v.client) || !a.uint_field(3, v.session) || !a.uint_field(4, v.epoch) ||
        !a.uint_field(5, v.request) || !a.uint_field(6, v.expected_revision)) return false;
    if (!visit_body(a, v)) return false;
    if (!seq(a, 9, Schema::Extension, v.extensions, a.state.limits.extensions, extension))
        return false;
    if ((!a.state.reading || a.state.materializing) && !unique_extension_tags(v.extensions)) return a.state.fail();
    return true;
}
class Validator final : public Archive {
  public:
    explicit Validator(Context &c) : Archive(c) {
        state.validating = true;
    }
    bool uint_field(unsigned, std::uint64_t &) noexcept override {
        return true;
    }
    bool int_field(unsigned, std::int64_t &) noexcept override {
        return true;
    }
    bool bool_field(unsigned, bool &) noexcept override {
        return true;
    }
    bool text_field(unsigned, std::string_view &v, bool binary) noexcept override {
        return state.store(v, binary ? state.limits.extension_bytes : state.limits.string_bytes, binary);
    }
    bool optional_uint(unsigned, std::uint64_t &, bool &) noexcept override {
        return true;
    }
    bool optional_int(unsigned, std::int64_t &, bool &) noexcept override {
        return true;
    }
    bool object(unsigned, Schema, void *p, Visit f, bool *present) noexcept override {
        return present && !*present ? true : f(*this, p);
    }
    bool sequence_size(unsigned, std::size_t &) noexcept override {
        return true;
    }
    bool sequence(unsigned, Schema, void *p, std::size_t n, ElementVisit f) noexcept override {
        for (std::size_t i = 0; i < n; ++i)
            if (!f(*this, p, i))
                return false;
        return true;
    }
};
} // namespace detail
Result validate(const Message &m, const Limits &limits) noexcept {
    detail::Context c{limits, {}};
    detail::Validator a(c);
    Message v = m;
    if (limits.depth > HardDepth)
        return {Status::InvalidArgument};
    if (!detail::visit_message(a, &v))
        return {c.error};
    Result result;
    result.required_arena = c.used;
    result.required_text = c.text;
    return result;
}
} // namespace bdsp_protocol::numeric_control
