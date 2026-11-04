#pragma once
#include <cstddef>
#include <limits>
#include <vector>
namespace presentation {
// Checked addressable geometry; no fixed history-length or matrix-cell quota.
struct NumericPlotLimits {
    static constexpr std::size_t bytes=std::numeric_limits<std::ptrdiff_t>::max();
    static constexpr std::size_t rows=bytes/sizeof(std::vector<float>), columns=bytes/sizeof(float), cells=columns;
    static bool accepts(std::size_t height,std::size_t width) noexcept {
        return height<=rows && width<=columns && (!width || height<=cells/width);
    }
};
}
