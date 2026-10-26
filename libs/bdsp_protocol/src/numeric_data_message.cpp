#include "text_validation.h"
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
        {Axis::Declared, 5}, {Axis::Component, 6})
SYMBOLS(Quantity, quantities, {Quantity::Unspecified, 0}, {Quantity::Amplitude, 1},
        {Quantity::SpectrumMagnitude, 2}, {Quantity::MelEnergy, 3}, {Quantity::FilledFrameCount, 4},
        {Quantity::Packet, 5}, {Quantity::SpectrumComplex, 6}, {Quantity::SpectrumReal, 7},
        {Quantity::SpectrumImaginary, 8}, {Quantity::SpectrumPhase, 9}, {Quantity::SpectrumPeakAmplitude, 10},
        {Quantity::SpectrumPower, 11}, {Quantity::SpectrumPsd, 12}, {Quantity::SpectrumAmplitudeDb, 13},
        {Quantity::SpectrumPowerDb, 14}, {Quantity::SpectrumPsdDb, 15}, {Quantity::BroadbandEnergy, 16},
        {Quantity::BroadbandPower, 17}, {Quantity::BroadbandRms, 18}, {Quantity::BroadbandMaximum, 19},
        {Quantity::SpectralFlux, 20}, {Quantity::NoteSalience, 21}, {Quantity::SpectrumRmsAmplitude, 22},
        {Quantity::SpectrumBinPower, 23}, {Quantity::SpectrumWindowScaledMagnitude, 24},
        {Quantity::BroadbandBinPowerSum, 25}, {Quantity::BroadbandBinPowerMean, 26},
        {Quantity::BroadbandBinPowerRms, 27}, {Quantity::BroadbandBinPowerMaximum, 28}, {Quantity::BroadbandBinPowerFlux, 29})
SYMBOLS(NumericUnit, units, {NumericUnit::Unspecified, 0}, {NumericUnit::NormalizedAmplitude, 1},
        {NumericUnit::RawAmplitude, 2}, {NumericUnit::Frames, 3}, {NumericUnit::Hertz, 4},
        {NumericUnit::Seconds, 5}, {NumericUnit::Values, 6}, {NumericUnit::Radians, 7},
        {NumericUnit::SquaredAmplitude, 8}, {NumericUnit::SquaredAmplitudePerHertz, 9}, {NumericUnit::Decibels, 10})
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
bool text_valid(std::string_view text,std::size_t limit,bool empty=false) noexcept {
    return (empty || !text.empty()) && text.size()<=limit && text_detail::utf8_without_nul(text);
}
Status limits_status(const Limits& l) noexcept {
    if(l.frame.max_elements>800000 || l.frame.max_payload_bytes>800000 ||
        l.frame.max_metadata_bytes>65536 || l.frame.max_string_bytes>256 ||
        l.frame.max_coordinate_bytes>32768 || l.text_bytes>65536 || l.extension_bytes>4096 ||
        l.history_rows>2000 || l.identity_bytes>256)return Status::exhausted;
    if(!l.message_bytes || l.message_bytes>900000 || l.frame.max_planes>16)return Status::invalid_argument;
    return Status::ok;
}
Status envelope_status(const Message& m,const Limits& l) noexcept {
    if(!text_valid(m.endpoint.graph,l.identity_bytes) || m.endpoint.node<0 ||
        (m.endpoint.direction!=Direction::Input && m.endpoint.direction!=Direction::Output) ||
        !m.envelope.nonce || !m.envelope.session || !m.envelope.delivery_sequence ||
        !text_valid(m.stamp.stream_id,l.identity_bytes) ||
        (m.values!=ValueDomain::FiniteOnly && m.values!=ValueDomain::AnyIeee))return Status::invalid_argument;
    return Status::ok;
}
bool valid_kind(Kind kind) noexcept {
    return kind==Kind::Frame || kind==Kind::HistoryRow || kind==Kind::Snapshot ||
        kind==Kind::Query || kind==Kind::Empty || kind==Kind::Reset || kind==Kind::Unavailable;
}
std::uint32_t extension_length(std::span<const std::byte> bytes,std::size_t at) noexcept {
    std::uint32_t size=0;
    for(unsigned i=0;i<4;++i)size|=std::uint32_t(std::to_integer<unsigned>(bytes[at+2+i]))<<(8*i);
    return size;
}
unsigned extension_tag(std::span<const std::byte> bytes,std::size_t at) noexcept {
    return std::to_integer<unsigned>(bytes[at]) | (std::to_integer<unsigned>(bytes[at+1])<<8);
}
bool repeated_extension_tag(std::span<const std::byte> bytes,std::size_t end,unsigned tag) noexcept {
    for(std::size_t prior=0;prior<end;) {
        if(extension_tag(bytes,prior)==tag)return true;
        prior+=6+extension_length(bytes,prior);
    }
    return false;
}
Status extensions_status(std::span<const std::byte> bytes,std::size_t limit) noexcept {
    if(bytes.size()>limit)return Status::exhausted;
    for(std::size_t at=0;at<bytes.size();) {
        if(bytes.size()-at<6)return Status::malformed;
        const auto tag=extension_tag(bytes,at);
        const auto size=extension_length(bytes,at);
        if(tag<1024 || (tag&32768) || repeated_extension_tag(bytes,at,tag))return Status::malformed;
        at+=6;
        if(size>bytes.size()-at)return Status::malformed;
        at+=size;
    }
    return Status::ok;
}
bool contract_text_valid(const NumericContract& n,const Limits& l) noexcept {
    auto known=[](auto value){std::uint8_t id=0;return symbol_id(value,id);};
    bool valid=known(n.dtype) && known(n.shape.rank) && known(n.semantic) && known(n.history_element) &&
        known(n.quantity) && known(n.unit) && known(n.representation.encoding) &&
        known(n.representation.placement) && known(n.representation.layout) &&
        known(n.representation.normalization);
    for(unsigned axis=0;axis<2;++axis)
        valid=valid && known(n.shape.axes[axis]) && known(n.coordinates[axis].mode) &&
            known(n.coordinates[axis].unit);
    return valid && text_valid(n.clock.domain,l.frame.max_string_bytes,true) && n.representation.alignment<=8;
}
Status structured_status(const Message& m,const Limits& l,const NumericContract* numeric) noexcept {
    if(numeric)return m.structured.empty()?Status::ok:Status::invalid_argument;
    const auto& structured=std::get<StructuredContract>(m.contract);
    if(!text_valid(structured.schema,l.frame.max_string_bytes) ||
        !text_valid(structured.encoding,l.frame.max_string_bytes))return Status::invalid_argument;
    if(structured.schema=="wled-audio-sync" && (structured.version!=1 ||
        structured.encoding!="audio-sync-44-le" || structured.payload_bytes.minimum!=44 ||
        structured.payload_bytes.maximum!=44))return Status::incompatible_contract;
    if(m.kind!=Kind::Frame && m.kind!=Kind::Unavailable)return Status::incompatible_contract;
    if(!m.numeric.storage.empty() || !m.numeric.planes.empty() ||
        !m.numeric.ring.silence_mask.empty() || !m.numeric.coordinates.axes[0].empty() ||
        !m.numeric.coordinates.axes[1].empty())return Status::invalid_argument;
    return Status::ok;
}
Status availability_status(const Message& m) noexcept {
    if(m.kind!=Kind::Unavailable)return m.stamp.available?Status::ok:Status::invalid_argument;
    if(m.stamp.available || !m.numeric.storage.empty() || !m.structured.empty() ||
        !m.numeric.planes.empty() || m.numeric.ring.capacity || m.numeric.ring.oldest ||
        !m.numeric.ring.silence_mask.empty() || !m.numeric.coordinates.axes[0].empty() ||
        !m.numeric.coordinates.axes[1].empty() || m.numeric.semantic_silence)return Status::invalid_argument;
    return Status::ok;
}
bool marker_kind(Kind kind) noexcept {
    return kind==Kind::Query || kind==Kind::Empty || kind==Kind::Reset;
}
Status marker_status(const Message& m,const NumericContract* numeric) noexcept {
    if(!numeric || (numeric->shape.rank!=Rank::Matrix && numeric->shape.rank!=Rank::Vector) || !m.numeric.storage.empty() ||
        !m.numeric.planes.empty() || m.numeric.ring.capacity || m.numeric.ring.oldest ||
        m.numeric.semantic_silence || !m.numeric.ring.silence_mask.empty() ||
        !m.numeric.coordinates.axes[0].empty() || !m.numeric.coordinates.axes[1].empty() ||
        (m.kind==Kind::Query && !m.envelope.request))return Status::invalid_argument;
    return Status::ok;
}
Status numeric_frame_status(const Message& m,const Limits& l,const NumericContract& numeric) noexcept {
    for(const auto& plane:m.numeric.planes)
        if(plane.offset%numeric.representation.alignment ||
            plane.stride_bytes[0]%numeric.representation.alignment ||
            plane.stride_bytes[1]%numeric.representation.alignment)return Status::invalid_argument;
    auto relaxed=numeric;
    relaxed.representation.alignment=1;
    auto frame=m.frame_view();
    auto& view=std::get<NumericView>(frame.payload);
    view.contract=&relaxed;
    if(const auto status=validate_view(frame,l.frame);status!=Status::ok)return status;
    return validate_values(view,m.values,l.frame);
}
Status payload_status(const Message& m,const Limits& l,const NumericContract* numeric) noexcept {
    if(marker_kind(m.kind))return marker_status(m,numeric);
    if(m.kind==Kind::Unavailable)return Status::ok;
    if(numeric)return numeric_frame_status(m,l,*numeric);
    return validate_view(m.frame_view(),l.frame);
}
Status history_status(const Message& m,const Limits& l,const NumericContract* numeric) noexcept {
    if(!numeric || (m.kind!=Kind::Snapshot && !marker_kind(m.kind) &&
        !(m.kind==Kind::Unavailable && m.history.capacity)))return Status::ok;
    if(numeric->shape.rank!=Rank::Matrix && numeric->shape.rank!=Rank::Vector)return Status::incompatible_contract;
    if(!l.history_rows || numeric->shape.extents[0].maximum>l.history_rows)return Status::exhausted;
    if(m.history.capacity!=numeric->shape.extents[0].maximum ||
        m.history.filled>m.history.capacity || m.history.ordinal<m.history.filled)return Status::invalid_argument;
    if(m.kind==Kind::Snapshot && (m.numeric.actual[0]!=m.history.capacity ||
        m.history.filled>m.numeric.actual[0] || m.numeric.ring.capacity!=m.history.capacity))return Status::invalid_argument;
    if((m.kind==Kind::Empty || m.kind==Kind::Reset) && m.history.filled)return Status::invalid_argument;
    return Status::ok;
}
NumericContract history_row_contract(const NumericContract& history) {
    auto row=history;
    row.semantic=history.history_element;
    row.history_element=Semantic::Numeric;
    if(history.shape.rank==Rank::Matrix){
        row.shape={Rank::Vector,{history.shape.axes[1],Axis::None},{history.shape.extents[1],ExtentBounds{}}};
        row.coordinates={history.coordinates[1],AxisCoordinates{}};
    }else{
        row.shape={Rank::Scalar,{Axis::None,Axis::None},{ExtentBounds{},ExtentBounds{}}};
        row.coordinates={AxisCoordinates{},AxisCoordinates{}};
    }
    return row;
}
bool history_row_bounds_valid(const Message& m,const Limits& l,const NumericContract& history) noexcept {
    return contract_text_valid(history,l) && (history.shape.rank==Rank::Matrix || history.shape.rank==Rank::Vector) && l.history_rows &&
        m.history.capacity==history.shape.extents[0].maximum && m.history.capacity<=l.history_rows &&
        m.history.filled && m.history.filled<=m.history.capacity && m.history.ordinal>=m.history.filled;
}
Status history_row_payload_status(const Message& message,const Limits& limits,
                                  const NumericContract& contract) noexcept {
    std::size_t width=0;
    const auto status=storage_width(contract,width);
    if(status!=Status::ok)return status;
    if(message.numeric.actual[0]>limits.frame.max_payload_bytes/width/contract.channel_order.size() ||
       message.numeric.storage.size()!=message.numeric.actual[0]*width*contract.channel_order.size())
        return Status::invalid_argument;
    return Status::ok;
}

Status history_row_status(const Message& m,const Limits& l,const NumericContract* numeric) noexcept {
    if(!numeric || (numeric->shape.rank!=Rank::Vector && numeric->shape.rank!=Rank::Scalar) || m.numeric.ring.capacity ||
       m.numeric.ring.oldest || !m.numeric.ring.silence_mask.empty())return Status::incompatible_contract;
    if(const auto status=validate_contract(DataContract{m.history_contract},l.frame);status!=Status::ok)return status;
    const auto& history=m.history_contract;
    if(!history_row_bounds_valid(m,l,history))return Status::invalid_argument;
    if(!same_physical_contract(DataContract{history_row_contract(history)},m.contract))
        return Status::incompatible_contract;
    return history_row_payload_status(m,l,*numeric);
}
Status history_metadata_status(const Message& m,const Limits& l,const NumericContract* numeric) noexcept {
    if(const auto status=history_status(m,l,numeric);status!=Status::ok)return status;
    if(m.kind==Kind::HistoryRow)return history_row_status(m,l,numeric);
    if((m.kind==Kind::Frame || (m.kind==Kind::Unavailable && !m.history.capacity)) &&
        (m.history.filled || m.history.capacity || m.history.ordinal))return Status::invalid_argument;
    if(m.kind==Kind::Frame && m.numeric.ring.capacity)return Status::incompatible_contract;
    if(!m.history_contract.channel_order.empty())return Status::invalid_argument;
    return Status::ok;
}
Status header_envelope_status(const Message& message,const Limits& limits) noexcept {
    const auto budget=limits_status(limits);
    if(budget!=Status::ok)return budget;
    const auto envelope=envelope_status(message,limits);
    if(envelope!=Status::ok)return envelope;
    if(!valid_kind(message.kind))return Status::invalid_argument;
    if((message.kind==Kind::Query || message.kind==Kind::Snapshot) &&
       !message.envelope.request)return Status::invalid_argument;
    return extensions_status(message.extensions,limits.extension_bytes);
}

Status header_contract_status(const Message& m,const Limits& l) noexcept {
    const auto header=header_envelope_status(m,l);
    if(header!=Status::ok)return header;
    const auto contract=validate_contract(m.contract,l.frame);
    if(contract!=Status::ok)return contract;
    const auto* numeric=std::get_if<NumericContract>(&m.contract);
    if(m.kind==Kind::Snapshot && (!numeric || (numeric->shape.rank!=Rank::Matrix && numeric->shape.rank!=Rank::Vector)))
        return Status::incompatible_contract;
    if(numeric && !contract_text_valid(*numeric,l))return Status::invalid_argument;
    return Status::ok;
}
} // namespace
static signal_contracts::Status validate_data_body(const Message& message,const Limits& limits,
    const signal_contracts::NumericContract* numeric) noexcept {
    using signal_contracts::Status;
    const auto structured=structured_status(message,limits,numeric);
    if(structured!=Status::ok)return structured;
    const auto available=availability_status(message);
    if(available!=Status::ok)return available;
    const auto payload=payload_status(message,limits,numeric);
    if(payload!=Status::ok)return payload;
    return history_metadata_status(message,limits,numeric);
}

signal_contracts::Status validate(const Message& m,const Limits& l) noexcept {
    using namespace signal_contracts;
    if(!m.stamp.latency_seconds.denominator || m.stamp.latency_seconds.numerator<0 ||
       bool(m.reference.binding)!=bool(m.reference.contract))return Status::invalid_argument;
    const auto header=header_contract_status(m,l);
    if(header!=Status::ok)return header;
    const auto* numeric=std::get_if<NumericContract>(&m.contract);
    const auto body=validate_data_body(m,l,numeric);
    if(body!=Status::ok)return body;
    if(!marker_kind(m.kind) && m.kind!=Kind::Unavailable && !m.stamp.sequence)
        return Status::invalid_argument;
    return Status::ok;
}
} // namespace bdsp_protocol::numeric_data
