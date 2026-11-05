// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct SpectralWhiteningConfig {
    float frame_period_s=512.f/44100.f,relax_time_s=250,floor=1e-4f;
};
// Port of aubio's adaptive spectral whitening. Magnitudes only; phase and
// FFT framing remain explicit upstream operations. In-place use is supported.
class PreparedSpectralWhitening {
public:
    bool prepare(std::size_t bins,SpectralWhiteningConfig={});
    static bool storage_bytes(std::size_t bins,SpectralWhiteningConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return (peaks_.capacity()+next_.capacity()+staging_.capacity())*sizeof(float);}
    void reset() noexcept;
    // Source controls preserve peak history. reset() is deliberately separate.
    bool set_relax_time(float seconds) noexcept;
    bool set_floor(float) noexcept;
    float relax_time() const noexcept {return config_.relax_time_s;}
    float floor() const noexcept {return config_.floor;}
    bool process(std::span<const float>,std::span<float>) noexcept;
    std::span<const float> peaks() const noexcept {return peaks_;}
private:
    SpectralWhiteningConfig config_;
    float decay_=0;
    std::vector<float> peaks_,next_,staging_;
};
}
