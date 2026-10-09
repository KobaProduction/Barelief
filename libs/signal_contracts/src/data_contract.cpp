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
    Status validate_contract(const DataContract& data,const FrameLimits& l) noexcept {
        if(const auto* p=std::get_if<StructuredContract>(&data)) {
            if(p->schema.empty() || p->encoding.empty() || !p->version || p->schema.size()>l.max_string_bytes || p->encoding.size()>l.max_string_bytes || !p->payload_bytes.minimum || p->payload_bytes.minimum>p->payload_bytes.maximum)return Status::invalid_argument;
            if(p->payload_bytes.maximum>l.max_payload_bytes || p->schema.size()>l.max_metadata_bytes || p->encoding.size()>l.max_metadata_bytes-p->schema.size())return Status::exhausted;
            return Status::ok;
        }
        const auto& c=std::get<NumericContract>(data);
        const auto* d=dtype_descriptor(c.dtype);
        std::size_t width=0;
        if(!d)return Status::invalid_argument;
        auto s=storage_width(c,width);
        if(s!=Status::ok)return s;
        const auto& r=c.representation;
        if(!r.alignment || (r.alignment&(r.alignment-1)) || (r.placement!=BitPlacement::LeastSignificant && r.placement!=BitPlacement::MostSignificant) || (r.layout!=ChannelLayout::Planar && r.layout!=ChannelLayout::Interleaved))return Status::invalid_argument;
        if(r.valid_bits>d->bytes*8 || (d->ieee && r.valid_bits) || (d->ieee && r.placement!=BitPlacement::LeastSignificant))return Status::incompatible_contract;
        if(r.normalization!=Normalization::None && r.normalization!=Normalization::SignedUnit && r.normalization!=Normalization::UnsignedUnit && r.normalization!=Normalization::FloatUnit)return Status::invalid_argument;
        if((r.normalization==Normalization::FloatUnit && !d->ieee) || (r.normalization==Normalization::SignedUnit && (!d->signed_value || d->ieee)) || (r.normalization==Normalization::UnsignedUnit && (d->signed_value || d->ieee)))return Status::incompatible_contract;
        if(static_cast<unsigned>(c.quantity)>static_cast<unsigned>(Quantity::Packet) || static_cast<unsigned>(c.unit)>static_cast<unsigned>(NumericUnit::Values) || static_cast<unsigned>(c.semantic)>static_cast<unsigned>(Semantic::Numeric) || static_cast<unsigned>(c.history_element)>static_cast<unsigned>(Semantic::Numeric))return Status::invalid_argument;
        const auto count=c.channel_order.size();
        if(!count || count>l.max_planes)return Status::exhausted;
        for(std::size_t i=0;i<count;++i)for(std::size_t j=0;j<i;++j)if(c.channel_order[i]==c.channel_order[j])return Status::invalid_argument;
        if(c.shape.rank!=Rank::Scalar && c.shape.rank!=Rank::Vector && c.shape.rank!=Rank::Matrix)return Status::invalid_argument;
        const std::size_t rank=c.shape.rank==Rank::Scalar?0:c.shape.rank==Rank::Vector?1:2;
        if(!rank && (count!=1 || c.shape.extents[0]!=ExtentBounds{} || c.shape.extents[1]!=ExtentBounds{}))return Status::incompatible_contract;
        std::size_t elements=count;
        for(std::size_t i=0;i<2;++i) {
            const auto e=c.shape.extents[i];
            if(static_cast<unsigned>(c.shape.axes[i])>static_cast<unsigned>(Axis::Declared) || static_cast<unsigned>(c.coordinates[i].unit)>static_cast<unsigned>(NumericUnit::Values))return Status::invalid_argument;
            if(e.minimum>e.maximum || !e.maximum || e.maximum>std::numeric_limits<std::size_t>::max() || (i>=rank && (c.shape.axes[i]!=Axis::None || e!=ExtentBounds{})))return Status::invalid_argument;
            if(i<rank){if(e.maximum>std::numeric_limits<std::size_t>::max()/elements)return Status::overflow;
                elements*=static_cast<std::size_t>(e.maximum);
            }
            const auto& x=c.coordinates[i];
            if(x.mode!=CoordinateMode::None && x.mode!=CoordinateMode::Regular && x.mode!=CoordinateMode::ExplicitFloat64LE)return Status::invalid_argument;
            if(!x.origin.denominator || !x.step.denominator || (i>=rank && x.mode!=CoordinateMode::None))return Status::invalid_argument;
        }
        if(elements>l.max_elements)return Status::exhausted;
        if(!c.clock.tick_seconds.denominator || c.clock.tick_seconds.numerator<=0 || c.clock.domain.size()>l.max_string_bytes)return Status::invalid_argument;
        if(count>l.max_metadata_bytes/sizeof(std::uint32_t) || c.clock.domain.size()>l.max_metadata_bytes-count*sizeof(std::uint32_t))return Status::exhausted;
        if(c.quantity==Quantity::Packet)return Status::incompatible_contract;
        if(c.quantity==Quantity::FilledFrameCount && (c.shape.rank!=Rank::Scalar || (c.dtype!=DType::UInt32 && c.dtype!=DType::UInt64) || c.unit!=NumericUnit::Frames))return Status::incompatible_contract;
        return Status::ok;
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
