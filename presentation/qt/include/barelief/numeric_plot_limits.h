#pragma once
#include <cstddef>
namespace presentation {
// Presentation resource budget, independent of DSP admission and FFT policies.
struct NumericPlotLimits {
    static constexpr std::size_t rows=16384, columns=16384, cells=1048576;
    static bool accepts(std::size_t height,std::size_t width) noexcept {
        return height<=rows && width<=columns && (!width || height<=cells/width);
    }
};
}
