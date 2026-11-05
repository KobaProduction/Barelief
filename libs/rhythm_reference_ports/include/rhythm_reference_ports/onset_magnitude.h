// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/spectral_whitening.h>
#include <cstddef>
#include <span>
namespace rhythm_reference_ports {
struct OnsetMagnitudeConfig {SpectralWhiteningConfig whitening;bool whiten=false;float compression=0;};
// Explicit whitening -> source log(1+lambda*magnitude) preprocessing. Both
// bypass controls are warm and preserve peak history; reset_whitening is separate.
class PreparedOnsetMagnitude {
public:
    bool prepare(std::size_t bins,OnsetMagnitudeConfig={});
    static bool storage_bytes(std::size_t bins,OnsetMagnitudeConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return whitening_.workspace_bytes();}
    bool process(std::span<float> magnitude) noexcept;
    void reset_whitening() noexcept {whitening_.reset();}
    bool set_whitening(bool) noexcept;
    bool set_compression(float) noexcept;
    bool set_whitening_floor(float value) noexcept {return whitening_.set_floor(value);}
    bool set_whitening_relax_time(float value) noexcept {return whitening_.set_relax_time(value);}
    bool whitening_enabled() const noexcept {return config_.whiten;}
    float compression() const noexcept {return config_.compression;}
    std::span<const float> whitening_peaks() const noexcept {return whitening_.peaks();}
private:
    OnsetMagnitudeConfig config_;
    PreparedSpectralWhitening whitening_;
    std::size_t bins_=0;
};
}
