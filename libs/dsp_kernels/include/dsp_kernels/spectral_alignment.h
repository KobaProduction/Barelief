#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
// Spectral correlation alignment from madmom correlation_diff. The previous
// spectrum is an explicit input, so history/temporal lag are caller operations.
// Radius >=1, bins >2*radius. Border bins remain zero, as in the source.
// Corrects the Python-2-only midpoint division to an integer midpoint.
class PreparedSpectralAlignment {
public:
    static bool storage_bytes(std::size_t bins,std::size_t radius,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return (correlations_.capacity()+difference_.capacity())*sizeof(float);}
    bool prepare(std::size_t bins,std::size_t radius=1,bool positive=false);
    bool process(std::span<const float> current,std::span<const float> reference,
                 std::span<float> difference) noexcept;
    std::ptrdiff_t offset() const noexcept {return offset_;}
    std::span<const float> correlations() const noexcept {return correlations_;}
private:
    bool aligned_difference(std::span<const float>,std::span<const float>,std::ptrdiff_t) noexcept;
    std::size_t bins_=0,radius_=0;
    bool positive_=false;
    std::ptrdiff_t offset_=0;
    std::vector<float> correlations_,difference_;
};
}
