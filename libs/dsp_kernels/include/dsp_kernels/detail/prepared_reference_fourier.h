#pragma once
#include <cstddef>
#include <memory>
#include <span>
#include <vector>
namespace dsp_kernels::detail {
struct ReferenceFourierPlan;
// Pinned PocketFFT real plan. Immutable coefficients may be shared by clones;
// each instance owns its mutable packed data and execution scratch. No hot I/O,
// threads, allocations, transforms through Python, or implicit source scaling.
class PreparedReferenceFourier {
public:
    static bool storage_bytes(std::size_t,std::size_t&) noexcept;
    bool prepare(std::size_t);
    bool forward(std::span<const double>,std::span<double>) noexcept;
    bool inverse(std::span<const double>,std::span<double>) noexcept;
    std::size_t workspace_bytes() const noexcept;
private:
    std::shared_ptr<const ReferenceFourierPlan> plan_;
    std::vector<double> packed_,scratch_;
};
}
