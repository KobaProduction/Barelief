#pragma once
#include <domain/numeric/values.h>
#include <span>
#include <string_view>
namespace project_storage {
std::string encode_array_generator_values(std::span<const float>);
signal_contracts::Status parse_numeric_array(std::string_view,domain::NumericArray&);
}
