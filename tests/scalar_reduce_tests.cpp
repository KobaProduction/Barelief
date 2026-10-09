#include <dsp_kernels/scalar_reduce.h>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

namespace {
std::size_t allocations = 0;
void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(3); }
}
}
void* operator new(std::size_t size) {
    ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    std::abort();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main() {
    using namespace dsp_kernels;
    const std::array first{-3.f, -1.f};
    const std::array second{2.f};
    float output = 91.f;
    const auto baseline = allocations;
    require(scalar_reduce(ScalarReduction::MaximumAbsolute, first, second, {3}, output) == ScalarReduceStatus::Ok && output == 3.f,
            "maxabs includes both borrowed populations");
    require(scalar_reduce(ScalarReduction::SignedMean, first, second, {3}, output) == ScalarReduceStatus::Ok && std::abs(output + 2.f/3.f) < 1e-6f,
            "signed mean uses combined count, preserves negative sign");
    require(scalar_reduce(ScalarReduction::Rms, first, second, {3}, output) == ScalarReduceStatus::Ok && std::abs(output - std::sqrt(14.f/3.f)) < 1e-6f,
            "RMS includes both channels");
    const std::array zeros{0.f, 0.f};
    require(scalar_reduce(ScalarReduction::SignedMean, zeros, {}, {2}, output) == ScalarReduceStatus::Ok && output == 0.f,
            "first zero is valid");
    const std::array extremes{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    require(scalar_reduce(ScalarReduction::Rms, extremes, {}, {2}, output) == ScalarReduceStatus::Ok && output == std::numeric_limits<float>::max(),
            "RMS products accumulate in double");
    require(scalar_reduce(ScalarReduction::SignedMean, extremes, {}, {2}, output) == ScalarReduceStatus::Ok && output == 0.f,
            "large opposite values cancel in double");
    auto reject = [&](ScalarReduction op, std::span<const float> a, std::span<const float> b,
                      ScalarReduceLimits limits, ScalarReduceStatus expected) {
        output = -91.f;
        require(scalar_reduce(op, a, b, limits, output) == expected && output == -91.f,
                "rejected reduction must leave output unchanged");
    };
    reject(static_cast<ScalarReduction>(99), first, second, {3}, ScalarReduceStatus::InvalidOperation);
    reject(ScalarReduction::Rms, {}, {}, {3}, ScalarReduceStatus::EmptyInput);
    reject(ScalarReduction::Rms, first, second, {2}, ScalarReduceStatus::LimitExceeded);
    reject(ScalarReduction::Rms, first, {}, {0}, ScalarReduceStatus::LimitExceeded);
    const std::array nan{std::numeric_limits<float>::quiet_NaN()}, inf{std::numeric_limits<float>::infinity()};
    reject(ScalarReduction::MaximumAbsolute, first, nan, {3}, ScalarReduceStatus::NonFiniteInput);
    reject(ScalarReduction::SignedMean, inf, {}, {1}, ScalarReduceStatus::NonFiniteInput);
    reject(ScalarReduction::Rms, nan, {}, {1}, ScalarReduceStatus::NonFiniteInput);
    require(allocations == baseline, "reduction allocates on processing path");
    require(first[0] == -3.f && second[0] == 2.f, "input population changed");
    std::cout << "scalar_reduce bounded numeric tests passed\n";
}
