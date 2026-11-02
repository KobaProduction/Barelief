#pragma once
#include <dsp_kernels/dsp.h>
#include <span>
#include <vector>

namespace dsp_kernels::detail {
// Internal radix composition for transforms larger than one backend table.
// Leaves use the existing FFT; prepared twiddles join their exact DFTs.
// Inputs/output are distinct internal workspaces owned by the outer plan.
class PreparedBlockedFourier {
public:
    static bool storage_bytes(std::size_t samples,std::size_t& bytes) noexcept;
    bool prepare(std::size_t samples);
    bool process(std::span<const float> real,std::span<const float> imaginary,
                 std::span<float> interleaved) noexcept;
    std::size_t workspace_bytes() const noexcept {
        return leaf_.workspace_bytes()+permutation_.capacity()*sizeof(std::size_t)+
            (real_.capacity()+imaginary_.capacity()+transformed_.capacity()+twiddles_.capacity())*sizeof(float);
    }
private:
    bool configure(std::size_t);
    void tables();
    bool leaves(std::span<const float>,std::span<const float>,std::span<float>) noexcept;
    void load_leaf(std::span<const float>,std::span<const float>,std::size_t) noexcept;
    void merge(std::span<float>,std::size_t width) const noexcept;
    void butterfly(std::span<float>,std::size_t left,std::size_t right,std::size_t twiddle) const noexcept;
    FFT leaf_;
    std::size_t samples_=0,leaf_size_=0;
    std::vector<std::size_t> permutation_;
    std::vector<float> real_,imaginary_,transformed_,twiddles_;
};
}
