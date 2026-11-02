#pragma once
#include <dsp_kernels/window_math.h>
#include <dsp_kernels/numerical_precision.h>
#include <span>
#include <vector>

namespace dsp_kernels {
enum class TemporalPadding {Constant,LinearRamp,Edge,Reflect,Symmetric};
enum class TemporalTail {Drop,CoverEnd,CenterCoverEnd,OriginInside};
struct TemporalWindowConfig {
    std::size_t length=384,hop=1;
    WindowFunction function=WindowFunction::Hann;
    bool periodic=true,center=true;
    TemporalTail tail=TemporalTail::Drop;
    bool odd_center_ceil=false;
    bool empty_centered_frame=false; // Explicit STFT empty-even-window behavior.
    NumericalPrecision precision=NumericalPrecision::PortableFloat32;
};
// Windows an explicit oldest-first scalar series. No audio/sample-rate
// assumptions: length and hop are measured in input frames. Centering uses
// future context, as in the original offline algorithms.
// CoverEnd with center=false emits the first frame reaching/passing the end,
// padded with zeros, then stops. Drop retains complete-frame STFT semantics.
// OriginInside emits only centers/origins strictly before input end (ceil(N/hop)),
// retaining madmom normal framing without an extra exact-end frame.
// CenterCoverEnd retains the first center reaching/passing the end, provided
// the frame still starts before the end. odd_center_ceil selects ceil(N/2)
// left padding instead of floor(N/2), explicitly for odd-length source recipes.
class PreparedTemporalWindow {
public:
    // Explicit coefficients override the registered function/periodic recipe;
    // copied during preparation, so callable/array source windows remain usable.
    bool prepare(TemporalWindowConfig,std::span<const float> coefficients={},std::span<const double> reference_coefficients={});
    static bool storage_bytes(TemporalWindowConfig,std::span<const float> coefficients,std::size_t& bytes) noexcept;
    std::size_t frames(std::size_t input_frames) const noexcept;
    static bool frame_count(TemporalWindowConfig,std::size_t input_frames,std::size_t&) noexcept;
    // Warm coefficient replacement, preserving geometry and allocated storage.
    bool set_coefficients(std::span<const float>) noexcept;
    bool read(std::span<const float> input,std::size_t frame,std::span<float> output,
              TemporalPadding padding=TemporalPadding::Constant) const noexcept;
    // Explicit first input sample, independent of center/hop/tail scheduling.
    // Reuses the prepared window and padding; negative positions are valid.
    bool read_at(std::span<const float> input,std::ptrdiff_t first_sample,std::span<float> output,
                 TemporalPadding padding=TemporalPadding::Constant) const noexcept;
    const TemporalWindowConfig& config() const noexcept {return config_;}
    bool read_double(std::span<const float>,std::size_t,std::span<double>,TemporalPadding) const noexcept;
    bool read_double_at(std::span<const float>,std::ptrdiff_t,std::span<double>,TemporalPadding) const noexcept;
    std::span<const float> coefficients() const noexcept {return window_;}
    std::span<const double> reference_coefficients() const noexcept {return reference_window_;}
    bool uses_explicit_coefficients() const noexcept {return explicit_window_;}
    std::size_t workspace_bytes() const noexcept {return window_.capacity()*sizeof(float)+reference_window_.capacity()*sizeof(double);}
private:
    bool configure_reference(std::span<const float>,std::span<const double>);
    std::size_t geometry_frames(std::size_t) const noexcept;
    std::size_t center_offset() const noexcept;
    std::size_t center_cover_frames(std::size_t) const noexcept;
    float sample(std::span<const float>,std::ptrdiff_t,TemporalPadding) const noexcept;
    TemporalWindowConfig config_;
    bool explicit_window_=false;
    std::vector<float> window_;
    std::vector<double> reference_window_;
};
}
