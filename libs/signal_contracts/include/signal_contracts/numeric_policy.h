#pragma once
#include <signal_contracts/data_contract.h>
#include <bit>
#include <type_traits>
namespace signal_contracts {
// Exact tagged literals: never a double container for Int64/UInt64.
using NumericLiteral=std::variant<std::int64_t,std::uint64_t,float,double>;
// Structural literals preserve all IEEE bits, including inactive NaN payloads/-0.
// Active policy/value validity is checked by the selected D kernel at Prepare.
inline bool literal_equal(const NumericLiteral& a,const NumericLiteral& b) noexcept {
    if(a.valueless_by_exception() || b.valueless_by_exception() || a.index()!=b.index())return false;
    return std::visit([&](const auto& x) {
        using T=std::decay_t<decltype(x)>;const auto& y=std::get<T>(b);
        if constexpr(std::is_same_v<T,float>)return std::bit_cast<std::uint32_t>(x)==std::bit_cast<std::uint32_t>(y);
        else if constexpr(std::is_same_v<T,double>)return std::bit_cast<std::uint64_t>(x)==std::bit_cast<std::uint64_t>(y);
        else return x==y;
    },a);
}
inline Status validate_literal(const NumericLiteral& value) noexcept {
    return value.valueless_by_exception()?Status::malformed:Status::ok;
}
enum class Rounding { TowardZero, NearestTiesEven };
enum class Overflow { Reject, Saturate };
enum class PrecisionLoss { RejectInexact, AllowDeclared };
enum class NonFinite { Reject, ReplaceFinite, ClampFinite };
enum class ScaleMode { Identity, IntegerAffine, DyadicAffine, PcmNormalize, PcmDenormalize, Binary64Affine };
struct Dyadic { NumericLiteral numerator=std::int64_t{0}; std::int16_t exponent2=0; };
struct Scaling { ScaleMode mode=ScaleMode::Identity; Dyadic gain{std::int64_t{1},0}, offset{}; };
struct ConversionPolicy {
    Rounding rounding=Rounding::NearestTiesEven; Overflow overflow=Overflow::Reject;
    PrecisionLoss precision_loss=PrecisionLoss::RejectInexact; NonFinite nonfinite=NonFinite::Reject;
    Scaling scaling;
    NumericLiteral nan_replacement=std::int64_t{0}, negative_inf_replacement=std::int64_t{0}, positive_inf_replacement=std::int64_t{0};
};
} // namespace signal_contracts
