#pragma once
#include <signal_contracts/frame.h>
#include <array>
#include <variant>
namespace signal_contracts {
// Symbols only: these enum ordinals are not wire/config identifiers.
enum class DType { Int8, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64, Float32, Float64 };
using Byte = std::uint8_t;
struct DTypeDescriptor { DType type; std::uint8_t bytes, alignment; bool signed_value, ieee; };
std::span<const DTypeDescriptor> dtype_descriptors() noexcept;
const DTypeDescriptor* dtype_descriptor(DType) noexcept;
enum class Rank { Scalar, Vector, Matrix };
// Optional descriptive metadata. Never a data type or an operation admission gate.
enum class Semantic { Scalar, PCM, Window, Windowed, SpectrumMagnitude, MelEnergy, History, Numeric };
enum class Axis { None, Sample, Bin, Band, Time, Declared };
enum class NumericUnit { Unspecified, NormalizedAmplitude, RawAmplitude, Frames, Hertz, Seconds, Values };
enum class ValueDomain { FiniteOnly, AnyIeee };
enum class StorageEncoding { Native, FixedLittleEndian, FixedBigEndian, PackedSigned24LE, PackedSigned24BE };
enum class BitPlacement { LeastSignificant, MostSignificant };
enum class ChannelLayout { Planar, Interleaved };
// FloatUnit declares the normalized PCM interval [-1,+1]; None has no implicit scaling.
enum class Normalization { None, SignedUnit, UnsignedUnit, FloatUnit };
enum class CoordinateMode { None, Regular, ExplicitFloat64LE };
struct Ratio { std::int64_t numerator=0; std::uint64_t denominator=1; bool operator==(const Ratio&) const=default; };
struct ExtentBounds { std::uint64_t minimum=1, maximum=1; bool operator==(const ExtentBounds&) const=default; };
struct ShapeContract { Rank rank=Rank::Scalar; std::array<Axis,2> axes{Axis::None,Axis::None}; std::array<ExtentBounds,2> extents{}; bool operator==(const ShapeContract&) const=default; };
struct RepresentationContract {
    StorageEncoding encoding=StorageEncoding::Native;
    std::uint8_t valid_bits=0; // 0 = full width; packed24 requires 24.
    BitPlacement placement=BitPlacement::LeastSignificant;
    ChannelLayout layout=ChannelLayout::Planar;
    Normalization normalization=Normalization::None;
    std::size_t alignment=1;
    bool operator==(const RepresentationContract&) const=default;
};
struct AxisCoordinates { CoordinateMode mode=CoordinateMode::None; NumericUnit unit=NumericUnit::Unspecified; Ratio origin{}, step{}; bool operator==(const AxisCoordinates&) const=default; };
struct ClockContract { std::string_view domain; Ratio tick_seconds{1,1}; std::uint32_t sample_rate=0; };
struct NumericContract {
    DType dtype=DType::Float32; ShapeContract shape;
    Semantic semantic=Semantic::Scalar, history_element=Semantic::Numeric;
    Quantity quantity=Quantity::Unspecified; NumericUnit unit=NumericUnit::Unspecified;
    std::span<const std::uint32_t> channel_order; // unique IDs; count = plane count
    RepresentationContract representation;
    std::array<AxisCoordinates,2> coordinates{}; ClockContract clock;
};
struct StructuredContract { std::string_view schema; std::uint32_t version=0; std::string_view encoding; ExtentBounds payload_bytes; };
using DataContract=std::variant<NumericContract,StructuredContract>;
// Every bound supplied by caller; zero is restrictive, never a desktop default.
struct FrameLimits { std::size_t max_planes, max_elements, max_payload_bytes, max_metadata_bytes, max_string_bytes, max_coordinate_bytes; };
Status validate_contract(const DataContract&,const FrameLimits&) noexcept;
Status storage_width(const NumericContract&,std::size_t& out) noexcept;
bool same_contract(const DataContract&,const DataContract&) noexcept;
// Exact physical metadata/storage, ignoring the optional descriptive semantic tags.
bool same_physical_contract(const DataContract&,const DataContract&) noexcept;
// Primitive storage compatibility. Descriptive axes/quantity/semantic/clock
// metadata are preserved by adapters; this function performs no reinterpretation.
bool same_numeric_storage(const NumericContract&,const NumericContract&) noexcept;
} // namespace signal_contracts
