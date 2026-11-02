#pragma once
#include <dsp_kernels/dsp.h>
#include <dsp_kernels/fourier_backend.h>
#include <dsp_kernels/detail/blocked_fourier.h>
#include <span>
#include <vector>

namespace dsp_kernels {
// Raw complex DFT/IDFT. Arbitrary lengths use Bluestein convolution over the
// existing FFT engine; powers of two use that engine directly. Inverse includes
// 1/N. Plan storage is prepared; processing never allocates. This is numerical
// infrastructure, without audio windows, timing, magnitude scaling or histories.
class PreparedFourierTransform {
public:
    static bool storage_bytes(std::size_t samples,std::size_t& bytes,const FourierBackendFactory* =nullptr) noexcept;
    bool prepare(std::size_t samples,const FourierBackendFactory* =nullptr);
    bool process(std::span<const float> real,std::span<const float> imaginary,
                 std::span<float> interleaved,bool inverse=false) noexcept;
    std::size_t samples() const noexcept {return samples_;}
    // Owned prepared payload, excluding object/allocator overhead and shared tables.
    std::size_t workspace_bytes() const noexcept;
private:
    bool configure(std::size_t samples);
    bool configure_backend(std::size_t,const FourierBackendFactory&);
    bool process_backend(std::span<const float>,std::span<const float>,std::span<float>,bool) noexcept;
    bool prepare_chirp();
    bool transform(std::span<const float>,std::span<const float>,std::span<float>) noexcept;
    bool forward() noexcept;
    bool convolve() noexcept;
    bool tiny() noexcept;
    void load(std::span<const float>,std::span<const float>,bool) noexcept;
    void finish(std::span<float>,bool) noexcept;
    FFT fft_;
    detail::PreparedBlockedFourier blocked_;
    PreparedFourierBackend forward_backend_,inverse_backend_;
    std::size_t samples_=0,capacity_=0;
    bool chirped_=false;
    std::vector<float> real_,imaginary_,transformed_,result_,chirp_,kernel_;
};
}
