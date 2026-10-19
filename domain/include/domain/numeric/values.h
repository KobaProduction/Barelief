#pragma once
#include <signal_contracts/numeric_policy.h>
#include <vector>
#include <cstddef>
#include <string>
#include <variant>

namespace domain {
inline constexpr std::size_t max_numeric_array_values=32768;
struct NumericArray { std::vector<float> values; bool operator==(const NumericArray&) const = default; };
using ProcessingParameterValue = std::variant<signal_contracts::NumericLiteral, bool, std::string, NumericArray>;
}
