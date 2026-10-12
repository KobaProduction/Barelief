#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The index stream preserves the original expanded mesh order. Positions and
// smooth normals are shared per sampled point; neither winding nor diagonals
// depend on camera orientation or signal values.
inline bool spectrogramMeshTopology(std::size_t rows, std::size_t columns,
                                    std::vector<std::uint32_t> &indices) {
    constexpr std::size_t maximumPoints = 1048576;
    if (rows > maximumPoints || columns > maximumPoints ||
        (rows && columns > maximumPoints / rows)) return false;
    const auto points = rows * columns;
    indices.clear();
    if (!points) return true;
    if (rows > 1 && columns > 1) {
        indices.reserve((rows - 1) * (columns - 1) * 6);
        for (std::size_t row = 0; row + 1 < rows; ++row)
            for (std::size_t column = 0; column + 1 < columns; ++column) {
                const auto a = std::uint32_t(row * columns + column);
                const auto b = std::uint32_t(a + columns);
                indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
    } else if (points == 1) indices.push_back(0);
    else {
        indices.reserve((points - 1) * 2);
        for (std::uint32_t point = 0; point + 1 < points; ++point)
            indices.insert(indices.end(), {point, point + 1});
    }
    return true;
}
