#pragma once
#include <array>
#include <cstddef>
#include <string_view>
namespace signal_contracts {
enum class WindowFunction { Hann, Hamming, Blackman, Rectangular, Bartlett, BlackmanHarris, FlatTop };
inline constexpr std::array<std::string_view,7> window_function_names{
    "hann","hamming","blackman","rectangular","bartlett","blackman_harris","flat_top"};
inline std::string_view window_function_name(WindowFunction function) noexcept {
    const auto index=std::size_t(function);
    return index<window_function_names.size()?window_function_names[index]:std::string_view{};
}
inline bool parse_window_function(std::string_view name,WindowFunction& out) noexcept {
    for(std::size_t i=0;i<window_function_names.size();++i)if(name==window_function_names[i]){out=WindowFunction(i);return true;}
    return false;
}
}
