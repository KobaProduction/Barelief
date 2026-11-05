// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <dsp_kernels/fourier_backend.h>
#include <rhythm_reference_ports/scalar_history.h>
#include <rhythm_reference_ports/onset_recipes.h>
#include <rhythm_reference_ports/reference_window.h>
#include <cstdint>
#include <vector>
namespace rhythm_reference_ports::detail {
// BTrack's explicit mixed source arithmetic: Double window/product -> Float32
// complex FFT -> Double polar/ODF history/reduction. One owned analysis kernel
// preserves the source's separate histories across warm method changes.
class PreparedPredictiveReferenceFrontend {
public:
    static bool storage_bytes(std::uint32_t frame,std::uint32_t hop,PredictiveOnsetMethod,PredictiveWindowShape,std::size_t&,
                              const dsp_kernels::FourierBackendFactory*) noexcept;
    bool prepare(std::uint32_t frame,std::uint32_t hop,PredictiveOnsetMethod,PredictiveWindowShape,const dsp_kernels::FourierBackendFactory*);
    // Explicit graph boundary after the source FFT: preserve Double polar and
    // ODF histories instead of round-tripping them through Float32 ports.
    static bool window_coefficients(std::span<double>,PredictiveWindowShape) noexcept;
    static bool complex_storage_bytes(std::size_t frame,std::size_t&) noexcept;
    bool prepare_complex(std::size_t frame);
    bool process_complex(std::span<const float>,float&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool process_hop(std::span<const float>,std::span<float> complex,
                     std::span<float> magnitude,std::span<float> phase,float&) noexcept;
    void restart() noexcept;
    bool set_method(PredictiveOnsetMethod) noexcept;
    double value() const noexcept {return value_;}
    std::span<const float> raw_pcm_history() const noexcept {return history_.values();}
    std::span<const float> fft_input() const noexcept {return input_;}
    std::span<const double> window() const noexcept {return window_;}
    std::span<const double> magnitude() const noexcept {return magnitude_;}
    std::span<const double> phase() const noexcept {return phase_;}
private:
    bool configure(std::uint32_t,std::uint32_t,PredictiveOnsetMethod,PredictiveWindowShape,const dsp_kernels::FourierBackendFactory&);
    void window_frame() noexcept;
    bool polar(std::span<const float>) noexcept;
    double distance(std::size_t) const noexcept;
    double contribution(std::size_t) const noexcept;
    double phase_deviation(std::size_t) const noexcept;
    bool energy(float&) noexcept;
    bool publish(double,float&) noexcept;
    bool evaluate(float&) noexcept;
    void commit(std::span<float>,std::span<float>) noexcept;
    void commit_history() noexcept;
    dsp_kernels::PreparedFourierBackend fft_;
    ScalarHistory history_;
    std::vector<float> input_;
    std::vector<double> window_,magnitude_,phase_,previous_magnitude_,previous_phase_,older_phase_;
    std::uint32_t hop_=0;
    PredictiveOnsetMethod method_=PredictiveOnsetMethod::PositiveComplexDifference;
    double value_=0,previous_energy_=0;
};
}
