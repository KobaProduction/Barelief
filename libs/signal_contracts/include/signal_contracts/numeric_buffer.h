#pragma once
#include <signal_contracts/data_contract.h>
#include <cstring>
#include <type_traits>

namespace signal_contracts {
struct PlaneLayout { std::size_t offset=0; std::array<std::size_t,2> stride_bytes{}; };
struct RingRows { std::size_t capacity=0,oldest=0; std::span<const std::byte> silence_mask; };
struct ElementIndex { std::size_t plane=0,row=0,column=0; };
// Physical access only. No clocks, coordinates, semantic labels, units or IDs.
struct NumericBufferShape {
    Rank rank=Rank::Scalar;
    std::array<ExtentBounds,2> extents{};
    bool operator==(const NumericBufferShape&) const=default;
};
struct NumericBufferContract {
    DType dtype=DType::Float32;
    NumericBufferShape shape;
    RepresentationContract representation;
    std::size_t plane_count=1;
    NumericBufferContract view() const noexcept {return *this;}
    bool operator==(const NumericBufferContract&) const=default;
};
struct NumericBufferView {
    const NumericBufferContract* contract=nullptr;
    std::span<const std::byte> storage;
    std::span<const PlaneLayout> planes;
    std::array<std::size_t,2> actual{1,1};
    RingRows ring;
    bool semantic_silence=false;
};
struct MutableNumericBufferView {
    const NumericBufferContract* contract=nullptr;
    std::span<std::byte> storage;
    std::span<const PlaneLayout> planes;
    std::array<std::size_t,2> actual{1,1};
    RingRows ring;
    NumericBufferView view() const noexcept {return {contract,storage,planes,actual,ring};}
};
Status storage_width(const NumericBufferContract&,std::size_t&) noexcept;
Status validate_contract(const NumericBufferContract&,const FrameLimits&) noexcept;
Status required_storage(const NumericBufferContract&,std::span<const PlaneLayout>,std::size_t&) noexcept;
Status validate_view(const NumericBufferView&,const FrameLimits&) noexcept;
Status validate_mutable(const MutableNumericBufferView&,const FrameLimits&) noexcept;
Status validate_values(const NumericBufferView&,ValueDomain,const FrameLimits&) noexcept;
Status fill_silence(MutableNumericBufferView&,const FrameLimits&) noexcept;
Status load_bits(const NumericBufferView&,ElementIndex,DType,std::uint64_t&) noexcept;
Status store_bits(MutableNumericBufferView&,ElementIndex,DType,std::uint64_t) noexcept;
inline bool same_physical_contract(const NumericBufferContract& a,const NumericBufferContract& b) noexcept {return a==b;}
inline bool same_contract(const NumericBufferContract& a,const NumericBufferContract& b) noexcept {return a==b;}

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
template<class T,class View> Status load_element(const View& view,ElementIndex index,T& out) noexcept {
    std::uint64_t bits=0;const auto status=load_bits(view,index,dtype_of<T>(),bits);if(status!=Status::ok)return status;
    if constexpr(sizeof(T)==8)std::memcpy(&out,&bits,8);
    else if constexpr(sizeof(T)==4){auto value=std::uint32_t(bits);std::memcpy(&out,&value,4);}
    else if constexpr(sizeof(T)==2){auto value=std::uint16_t(bits);std::memcpy(&out,&value,2);}
    else {auto value=std::uint8_t(bits);std::memcpy(&out,&value,1);}return Status::ok;
}
template<class T,class View> Status store_element(View& view,ElementIndex index,T value) noexcept {
    std::uint64_t bits=0;
    if constexpr(sizeof(T)==8)std::memcpy(&bits,&value,8);
    else if constexpr(sizeof(T)==4){std::uint32_t encoded;std::memcpy(&encoded,&value,4);bits=encoded;}
    else if constexpr(sizeof(T)==2){std::uint16_t encoded;std::memcpy(&encoded,&value,2);bits=encoded;}
    else {std::uint8_t encoded;std::memcpy(&encoded,&value,1);bits=encoded;}
    return store_bits(view,index,dtype_of<T>(),bits);
}
} // namespace signal_contracts
