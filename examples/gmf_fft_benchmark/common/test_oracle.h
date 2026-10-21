#pragma once
#include <vector>
#include <complex>
#include <algorithm>
#include <cstdint>
namespace gmf_test {
inline void bit_reverse(std::vector<std::complex<double>> &values) {
    const auto n = values.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n / 2;
        while (j & bit) {
            j ^= bit;
            bit /= 2;
        }
        j ^= bit;
        if (i < j)
            std::swap(values[i], values[j]);
    }
}
inline void reference_group(std::vector<std::complex<double>> &values, size_t offset, size_t length,
                            std::complex<double> step) {
    std::complex<double> w = 1;
    for (size_t j = 0; j < length / 2; ++j) {
        auto u = values[offset + j], v = values[offset + j + length / 2] * w;
        values[offset + j] = u + v;
        values[offset + j + length / 2] = u - v;
        w *= step;
    }
}
inline std::vector<std::complex<double>> reference(const std::vector<int16_t> &input) {
    const auto n = input.size();
    std::vector<std::complex<double>> values(n);
    for (size_t i = 0; i < n; ++i)
        values[i] = input[i];
    bit_reverse(values);
    for (size_t length = 2; length <= n; length *= 2) {
        auto step = std::polar(1., -6.2831853071795864769 / length);
        for (size_t i = 0; i < n; i += length)
            reference_group(values, i, length, step);
    }
    return values;
}
} // namespace gmf_test
