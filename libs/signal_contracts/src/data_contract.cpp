#include <signal_contracts/data_contract.h>
#include <algorithm>
#include <climits>
#include <bit>
#include <limits>
namespace signal_contracts {
    namespace {
        static_assert(CHAR_BIT==8 && (std::endian::native==std::endian::little || std::endian::native==std::endian::big));
        static_assert(sizeof(float)==4 && sizeof(double)==8 && std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);
        static_assert(std::bit_cast<std::uint64_t>(std::int64_t{-1})==UINT64_MAX);
        constexpr DTypeDescriptor types[]{
            {DType::Int8,1,alignof(std::int8_t),true,false},{DType::UInt8,1,alignof(std::uint8_t),false,false},
            {DType::Int16,2,alignof(std::int16_t),true,false},{DType::UInt16,2,alignof(std::uint16_t),false,false},
            {DType::Int32,4,alignof(std::int32_t),true,false},{DType::UInt32,4,alignof(std::uint32_t),false,false},
            {DType::Int64,8,alignof(std::int64_t),true,false},{DType::UInt64,8,alignof(std::uint64_t),false,false},
            {DType::Float32,4,alignof(float),true,true},{DType::Float64,8,alignof(double),true,true}};
        bool encoding_valid(StorageEncoding e){return e==StorageEncoding::Native || e==StorageEncoding::FixedLittleEndian || e==StorageEncoding::FixedBigEndian || e==StorageEncoding::PackedSigned24LE || e==StorageEncoding::PackedSigned24BE;
        }
        bool numeric_equal(const NumericContract& a,const NumericContract& b) {
            return a.dtype==b.dtype && a.shape==b.shape && a.semantic==b.semantic && a.history_element==b.history_element && a.quantity==b.quantity && a.unit==b.unit && a.representation==b.representation && a.coordinates==b.coordinates && a.clock.domain==b.clock.domain && a.clock.tick_seconds==b.clock.tick_seconds && a.clock.sample_rate==b.clock.sample_rate && std::equal(a.channel_order.begin(),a.channel_order.end(),b.channel_order.begin(),b.channel_order.end());
        }
        Status validate_structured(const StructuredContract& value,const FrameLimits& limits) {
            if(value.schema.empty() || value.encoding.empty() || !value.version || value.schema.size()>limits.max_string_bytes || value.encoding.size()>limits.max_string_bytes || !value.payload_bytes.minimum || value.payload_bytes.minimum>value.payload_bytes.maximum)return Status::invalid_argument;
            if(value.payload_bytes.maximum>limits.max_payload_bytes || value.schema.size()>limits.max_metadata_bytes || value.encoding.size()>limits.max_metadata_bytes-value.schema.size())return Status::exhausted;
            return Status::ok;
        }
        Status validate_normalization(Normalization normalization,const DTypeDescriptor& dtype) {
            if(normalization!=Normalization::None && normalization!=Normalization::SignedUnit && normalization!=Normalization::UnsignedUnit && normalization!=Normalization::FloatUnit)return Status::invalid_argument;
            if((normalization==Normalization::FloatUnit && !dtype.ieee) || (normalization==Normalization::SignedUnit && (!dtype.signed_value || dtype.ieee)) || (normalization==Normalization::UnsignedUnit && (dtype.signed_value || dtype.ieee)))return Status::incompatible_contract;
            return Status::ok;
        }
        Status validate_representation(const RepresentationContract& value,const DTypeDescriptor& dtype) {
            if(!value.alignment || (value.alignment&(value.alignment-1)) || (value.placement!=BitPlacement::LeastSignificant && value.placement!=BitPlacement::MostSignificant) || (value.layout!=ChannelLayout::Planar && value.layout!=ChannelLayout::Interleaved))return Status::invalid_argument;
            if(value.valid_bits>dtype.bytes*8 || (dtype.ieee && value.valid_bits) || (dtype.ieee && value.placement!=BitPlacement::LeastSignificant))return Status::incompatible_contract;
            return validate_normalization(value.normalization,dtype);
        }
        Status validate_numeric_tags(const NumericContract& value) {
            if(static_cast<unsigned>(value.quantity)>static_cast<unsigned>(Quantity::SpectrumWindowScaledMagnitude) || static_cast<unsigned>(value.unit)>static_cast<unsigned>(NumericUnit::Decibels) || static_cast<unsigned>(value.semantic)>static_cast<unsigned>(Semantic::Numeric) || static_cast<unsigned>(value.history_element)>static_cast<unsigned>(Semantic::Numeric))return Status::invalid_argument;
            return Status::ok;
        }
        Status validate_channel_order(const NumericContract& value,const FrameLimits& limits) {
            const auto channels=value.channel_order;
            if(channels.empty() || channels.size()>limits.max_planes)return Status::exhausted;
            for(std::size_t i=0;i<channels.size();++i)for(std::size_t j=0;j<i;++j)if(channels[i]==channels[j])return Status::invalid_argument;
            return Status::ok;
        }
        Status validate_numeric_labels(const NumericContract& value,const FrameLimits& limits) {
            const auto status=validate_numeric_tags(value);if(status!=Status::ok)return status;
            return validate_channel_order(value,limits);
        }
        Status validate_axis_coordinates(const AxisCoordinates& value,bool used) {
            if(value.mode!=CoordinateMode::None && value.mode!=CoordinateMode::Regular && value.mode!=CoordinateMode::ExplicitFloat64LE)return Status::invalid_argument;
            if(!value.origin.denominator || !value.step.denominator || (!used && value.mode!=CoordinateMode::None))return Status::invalid_argument;
            return Status::ok;
        }
        Status validate_numeric_axis(const NumericContract& value,std::size_t axis,std::size_t rank,std::size_t& elements) {
            const auto extent=value.shape.extents[axis];
            if(static_cast<unsigned>(value.shape.axes[axis])>static_cast<unsigned>(Axis::Component) || static_cast<unsigned>(value.coordinates[axis].unit)>static_cast<unsigned>(NumericUnit::Decibels))return Status::invalid_argument;
            if(extent.minimum>extent.maximum || !extent.maximum || extent.maximum>std::numeric_limits<std::size_t>::max() || (axis>=rank && (value.shape.axes[axis]!=Axis::None || extent!=ExtentBounds{})))return Status::invalid_argument;
            if(axis<rank) {
                if(extent.maximum>std::numeric_limits<std::size_t>::max()/elements)return Status::overflow;
                elements*=static_cast<std::size_t>(extent.maximum);
            }
            return validate_axis_coordinates(value.coordinates[axis],axis<rank);
        }
        Status validate_numeric_shape(const NumericContract& value,const FrameLimits& limits) {
            if(value.shape.rank!=Rank::Scalar && value.shape.rank!=Rank::Vector && value.shape.rank!=Rank::Matrix)return Status::invalid_argument;
            const std::size_t rank=value.shape.rank==Rank::Scalar?0:value.shape.rank==Rank::Vector?1:2;
            if(!rank && (value.channel_order.size()!=1 || value.shape.extents[0]!=ExtentBounds{} || value.shape.extents[1]!=ExtentBounds{}))return Status::incompatible_contract;
            std::size_t elements=value.channel_order.size();
            for(std::size_t axis=0;axis<2;++axis) {
                const auto status=validate_numeric_axis(value,axis,rank,elements);if(status!=Status::ok)return status;
            }
            return elements>limits.max_elements?Status::exhausted:Status::ok;
        }
        Status validate_numeric_metadata(const NumericContract& value,const FrameLimits& limits) {
            if(!value.clock.tick_seconds.denominator || value.clock.tick_seconds.numerator<=0 || value.clock.domain.size()>limits.max_string_bytes)return Status::invalid_argument;
            const auto count=value.channel_order.size();
            if(count>limits.max_metadata_bytes/sizeof(std::uint32_t) || value.clock.domain.size()>limits.max_metadata_bytes-count*sizeof(std::uint32_t))return Status::exhausted;
            if(value.quantity==Quantity::Packet)return Status::incompatible_contract;
            if(value.quantity==Quantity::FilledFrameCount && (value.shape.rank!=Rank::Scalar || (value.dtype!=DType::UInt32 && value.dtype!=DType::UInt64) || value.unit!=NumericUnit::Frames))return Status::incompatible_contract;
            return Status::ok;
        }
        Status validate_numeric_storage(const NumericContract& value) {
            const auto* dtype=dtype_descriptor(value.dtype);if(!dtype)return Status::invalid_argument;
            std::size_t width=0;const auto status=storage_width(value,width);if(status!=Status::ok)return status;
            return validate_representation(value.representation,*dtype);
        }
    }
    std::span<const DTypeDescriptor> dtype_descriptors() noexcept {return types;
    }
    const DTypeDescriptor* dtype_descriptor(DType t) noexcept {for(const auto& d:types)if(d.type==t)return &d;
        return nullptr;
    }
    Status storage_width(const NumericContract& c,std::size_t& out) noexcept {
        const auto* d=dtype_descriptor(c.dtype);
        if(!d || !encoding_valid(c.representation.encoding))return Status::invalid_argument;
        if(c.representation.valid_bits>d->bytes*8 || (d->ieee && c.representation.valid_bits) || (c.representation.placement!=BitPlacement::LeastSignificant && c.representation.placement!=BitPlacement::MostSignificant))return Status::incompatible_contract;
        const bool packed=c.representation.encoding==StorageEncoding::PackedSigned24LE || c.representation.encoding==StorageEncoding::PackedSigned24BE;
        if(packed && (c.dtype!=DType::Int32 || c.representation.valid_bits!=24 || c.representation.placement!=BitPlacement::LeastSignificant))return Status::incompatible_contract;
        out=packed?3:d->bytes;
        return Status::ok;
    }
    Status validate_contract(const DataContract& data,const FrameLimits& limits) noexcept {
    if(const auto* structured=std::get_if<StructuredContract>(&data))return validate_structured(*structured,limits);
    const auto& numeric=std::get<NumericContract>(data);
    auto status=validate_numeric_storage(numeric);if(status!=Status::ok)return status;
    status=validate_numeric_labels(numeric,limits);if(status!=Status::ok)return status;
    status=validate_numeric_shape(numeric,limits);if(status!=Status::ok)return status;
    return validate_numeric_metadata(numeric,limits);
}
    bool same_numeric_storage(const NumericContract& a,const NumericContract& b) noexcept {
        return a.dtype==b.dtype && a.shape.rank==b.shape.rank && a.shape.extents==b.shape.extents &&
            a.representation==b.representation && std::equal(a.channel_order.begin(),a.channel_order.end(),b.channel_order.begin(),b.channel_order.end());
    }
    bool same_physical_contract(const DataContract& a,const DataContract& b) noexcept {
        if(a.index()!=b.index())return false;
        if(const auto* numeric=std::get_if<NumericContract>(&a)){
            auto comparable=*numeric;const auto& other=std::get<NumericContract>(b);
            comparable.semantic=other.semantic;comparable.history_element=other.history_element;
            return numeric_equal(comparable,other);
        }
        return same_contract(a,b);
    }
    bool same_contract(const DataContract& a,const DataContract& b) noexcept {
        if(a.index()!=b.index())return false;
        if(const auto* p=std::get_if<NumericContract>(&a))return numeric_equal(*p,std::get<NumericContract>(b));
        const auto& x=std::get<StructuredContract>(a);
        const auto& y=std::get<StructuredContract>(b);
        return x.schema==y.schema && x.version==y.version && x.encoding==y.encoding && x.payload_bytes==y.payload_bytes;
    }
}
