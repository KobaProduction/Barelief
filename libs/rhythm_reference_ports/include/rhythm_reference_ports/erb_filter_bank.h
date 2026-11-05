// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct ErbFilterBankConfig {
    float sample_rate=44100,low_hz=80,high_hz=22050,width=1;
    std::size_t bands=40;
};
// Essentia ERBBands' frequency-domain gammatone coefficients, including its
// original repeated-pole expression. Input is linear; squaring for the power
// variant is a separate explicit operation. DC/Nyquist-inclusive uniform grid.
class PreparedErbFilterBank {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t bins,ErbFilterBankConfig,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t bins,ErbFilterBankConfig={});
    bool process(std::span<const float>,std::span<float>) const noexcept;
    std::span<const float> weights() const noexcept {return weights_;}
    std::span<const float> centers_hz() const noexcept {return centers_;}
private:
    bool configure(std::size_t,ErbFilterBankConfig);
    bool build_band(std::size_t);
    ErbFilterBankConfig config_;
    std::size_t bins_=0;
    std::vector<float> weights_,centers_;
    mutable std::vector<float> staging_;
};
}
