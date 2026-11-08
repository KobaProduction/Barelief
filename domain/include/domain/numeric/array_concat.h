#pragma once
#include <cstdint>
#include <optional>
namespace domain {
struct ResolvedProcessingNode;
// Stable selector values: first dimension, then second dimension.
enum class ArrayConcatAxis : std::uint8_t { First=0, Second=1 };
constexpr std::optional<ArrayConcatAxis> array_concat_axis(double value) noexcept {
    if(value==0)return ArrayConcatAxis::First;
    if(value==1)return ArrayConcatAxis::Second;
    return std::nullopt;
}
std::optional<ArrayConcatAxis> array_concat_axis(const ResolvedProcessingNode&) noexcept;
}
