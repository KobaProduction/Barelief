#pragma once
#include <signal_contracts/data_contract.h>
#include <signal_contracts/buffer_pool.h>
#include <cstring>
#include <type_traits>
namespace signal_contracts {
struct PlaneLayout { std::size_t offset=0; std::array<std::size_t,2> stride_bytes{}; };
struct RingRows { std::size_t capacity=0, oldest=0; std::span<const std::byte> silence_mask; };
struct CoordinateView { std::array<std::span<const std::byte>,2> axes; };
struct NumericView {
    const NumericContract* contract=nullptr; std::span<const std::byte> storage;
    std::span<const PlaneLayout> planes; std::array<std::size_t,2> actual{1,1};
    RingRows ring; CoordinateView coordinates; bool semantic_silence=false;
};
struct MutableNumericView {
    const NumericContract* contract=nullptr; std::span<std::byte> storage;
    std::span<const PlaneLayout> planes; std::array<std::size_t,2> actual{1,1};
    RingRows ring; CoordinateView coordinates;
    NumericView view() const noexcept {return {contract,storage,planes,actual,ring,coordinates};}
};
struct StructuredView { const StructuredContract* contract=nullptr; std::span<const std::byte> bytes; };
struct FrameStamp {
    std::uint64_t sequence=0,generation=0,epoch=0,graph_revision=0,source_sequence=0,producer_epoch=0;
    std::int64_t timestamp_ticks=0; std::string_view stream_id;
    bool available=true,discontinuity=false;
    std::int64_t sample_begin=0;
    std::uint64_t sample_count=0,latency_samples=0;
    bool has_sample_interval=false;
    Ratio latency_seconds{0,1}; // Exact rational nominal content delay; samples is diagnostic.
    // A scalar nominal delay cannot describe nonlinear-phase processing.
    bool frequency_dependent_latency=false;
};
struct DataFrameView { FrameStamp stamp; std::variant<NumericView,StructuredView> payload; };
struct ElementIndex { std::size_t plane=0,row=0,column=0; };
Status required_storage(const NumericContract&,std::span<const PlaneLayout>,std::size_t& out) noexcept;
Status validate_view(const DataFrameView&,const FrameLimits&) noexcept;
Status validate_mutable(const MutableNumericView&,const FrameLimits&) noexcept;
Status validate_values(const NumericView&,ValueDomain,const FrameLimits&) noexcept;
// Borrow one logical row: Matrix -> Vector; scalar Vector History -> Scalar.
Status history_row(const NumericView&,std::size_t row,NumericView& out,NumericContract& row_contract,
                   std::span<PlaneLayout> row_planes) noexcept;
// Exact dtype access; packed24 logical Int32. Internal bits helpers never cast bytes to T*.
Status load_bits(const NumericView&,ElementIndex,DType,std::uint64_t& out) noexcept;
Status store_bits(MutableNumericView&,ElementIndex,DType,std::uint64_t bits) noexcept;
Status fill_silence(MutableNumericView&,const FrameLimits&) noexcept;
template<class T> constexpr DType signed_integer_dtype() {
    if constexpr(std::is_same_v<T,std::int8_t>)return DType::Int8;
    else if constexpr(std::is_same_v<T,std::int16_t>)return DType::Int16;
    else if constexpr(std::is_same_v<T,std::int32_t>)return DType::Int32;
    else {static_assert(std::is_same_v<T,std::int64_t>);return DType::Int64;}
}
template<class T> constexpr DType unsigned_integer_dtype() {
    if constexpr(std::is_same_v<T,std::uint8_t>)return DType::UInt8;
    else if constexpr(std::is_same_v<T,std::uint16_t>)return DType::UInt16;
    else if constexpr(std::is_same_v<T,std::uint32_t>)return DType::UInt32;
    else {static_assert(std::is_same_v<T,std::uint64_t>);return DType::UInt64;}
}
template<class T> constexpr DType dtype_of() {
    if constexpr(std::is_integral_v<T> && std::is_signed_v<T>)return signed_integer_dtype<T>();
    else if constexpr(std::is_integral_v<T>)return unsigned_integer_dtype<T>();
    else if constexpr(std::is_same_v<T,float>)return DType::Float32;
    else {static_assert(std::is_same_v<T,double>);return DType::Float64;}
}
template<class T> Status load_element(const NumericView& v,ElementIndex i,T& out) noexcept {
    std::uint64_t bits=0;auto s=load_bits(v,i,dtype_of<T>(),bits);if(s!=Status::ok)return s;
    // Bit storage is host endian-independent. memcpy establishes a value in an existing T.
    if constexpr(sizeof(T)==8)std::memcpy(&out,&bits,8);
    else if constexpr(sizeof(T)==4){auto x=std::uint32_t(bits);std::memcpy(&out,&x,4);}
    else if constexpr(sizeof(T)==2){auto x=std::uint16_t(bits);std::memcpy(&out,&x,2);}
    else {auto x=std::uint8_t(bits);std::memcpy(&out,&x,1);}return Status::ok;
}
template<class T> Status store_element(MutableNumericView& v,ElementIndex i,T value) noexcept {
    std::uint64_t bits=0;
    if constexpr(sizeof(T)==8)std::memcpy(&bits,&value,8);
    else if constexpr(sizeof(T)==4){std::uint32_t x;std::memcpy(&x,&value,4);bits=x;}
    else if constexpr(sizeof(T)==2){std::uint16_t x;std::memcpy(&x,&value,2);bits=x;}
    else {std::uint8_t x;std::memcpy(&x,&value,1);bits=x;}
    return store_bits(v,i,dtype_of<T>(),bits);
}
struct RetentionRequirements { std::size_t payload_bytes=0,metadata_bytes=0,metadata_alignment=0; };
class OwnedDataFrame {
public:
    OwnedDataFrame()=default;
    OwnedDataFrame(const OwnedDataFrame&)=default; OwnedDataFrame& operator=(const OwnedDataFrame&)=default;
    OwnedDataFrame(OwnedDataFrame&&) noexcept=default; OwnedDataFrame& operator=(OwnedDataFrame&&) noexcept=default;
    DataFrameView view() const noexcept;
    OwnedDataFrame with_discontinuity() const noexcept {auto copy=*this;copy.stamp_.discontinuity=true;return copy;}
    explicit operator bool() const noexcept {return bool(payload_) && bool(metadata_);}
private:
    BufferHandle payload_,metadata_; DataContract contract_; FrameStamp stamp_;
    std::array<std::size_t,2> actual_{1,1}; std::size_t oldest_=0,capacity_=0; bool semantic_silence_=false;
    std::size_t planes_offset_=0,planes_count_=0,channels_offset_=0;
    std::size_t schema_offset_=0,encoding_offset_=0,clock_offset_=0,stream_offset_=0;
    std::size_t mask_offset_=0,mask_size_=0;
    std::array<std::size_t,2> coordinate_offsets_{},coordinate_sizes_{};
    friend Status retain_frame(const DataFrameView&,const FrameLimits&,BufferPool&,BufferPool&,OwnedDataFrame&) noexcept;
    friend struct FrameRetention;
};
Status retention_requirements(const DataFrameView&,const FrameLimits&,RetentionRequirements&) noexcept;
// Explicit snapshot operation; not a full-history-copy canonical processing callback.
// Disjoint aligned pool storage must outlive all owners; failure leaves out unchanged.
Status retain_frame(const DataFrameView&,const FrameLimits&,BufferPool& payload,BufferPool& metadata,OwnedDataFrame& out) noexcept;
} // namespace signal_contracts
