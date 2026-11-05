// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <dsp_kernels/fourier_backend.h>
#include <vector>
namespace rhythm_reference_ports::detail {
// Explicit BTrack source arithmetic around the existing Float32 complex FFT.
// The fixed source ACF length is 1024, with 512 onset/lag values. All semantic
// threshold/comb/transition state is Double; public adapters still publish F32.
class PreparedPredictiveReferenceComb {
public:
    static bool storage_bytes(std::size_t hop,std::size_t&,const dsp_kernels::FourierBackendFactory*) noexcept;
    bool prepare(std::size_t hop,const dsp_kernels::FourierBackendFactory*);
    std::size_t workspace_bytes() const noexcept;
    bool ready() const noexcept {return hop_!=0;}
    void reset() noexcept;
    bool set_hop_samples(std::size_t) noexcept;
    bool process(std::span<const float>,float& period) noexcept;
    bool set_tempo(float) noexcept;
    bool fix_tempo(float) noexcept;
    void release_tempo() noexcept {fixed_=false;}
    double bpm() const noexcept {return bpm_;}
    std::span<const double> thresholded_onsets() const noexcept {return onset_;}
    std::span<const double> autocorrelation() const noexcept {return acf_;}
    std::span<const double> response() const noexcept {return comb_;}
    std::span<const double> observations() const noexcept {return observation_;}
    std::span<const double> probabilities() const noexcept {return previous_;}
private:
    bool configure(std::size_t,const dsp_kernels::FourierBackendFactory&);
    void prepare_buffers();
    void prepare_weights() noexcept;
    void prepare_transitions() noexcept;
    void threshold(std::span<double>) noexcept;
    bool load() noexcept;
    bool correlate() noexcept;
    bool finish_correlation() noexcept;
    void filter() noexcept;
    void observe() noexcept;
    bool transition() noexcept;
    bool select(float&) noexcept;
    std::size_t tempo_index(float) const noexcept;
    dsp_kernels::PreparedFourierBackend forward_,inverse_;
    std::vector<float> input_,spectrum_;
    std::vector<double> onset_,acf_,threshold_,rayleigh_,comb_;
    std::vector<double> observation_,previous_,fixed_previous_,current_,transitions_;
    std::size_t hop_=0;
    bool fixed_=false;
    double bpm_=120;
};
}
