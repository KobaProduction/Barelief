#pragma once
#include <cstdint>
#include <initializer_list>
#include <optional>

namespace domain {
// Stable values in the MatrixCenter processing settings.
enum class MatrixCenterMethod : std::uint8_t { Mean = 0, Midrange = 1, ManualOffset = 2 };

constexpr std::optional<MatrixCenterMethod> matrix_center_method(double value) noexcept {
    using enum MatrixCenterMethod;
    for (const auto method : {Mean, Midrange, ManualOffset})
        if (value == static_cast<double>(method)) return method;
    return std::nullopt;
}
}
