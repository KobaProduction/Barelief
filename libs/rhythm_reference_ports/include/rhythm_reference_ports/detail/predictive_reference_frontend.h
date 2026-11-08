// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports::detail {
// Positive complex spectral difference only; window/FFT/PCM accumulation are graph stages.
class PreparedPredictiveReferenceFrontend {
public:
    static bool complex_storage_bytes(std::size_t,std::size_t&) noexcept;
    bool prepare_complex(std::size_t);
    bool process_complex(std::span<const float>,float&) noexcept;
    void restart() noexcept;
    double value() const noexcept {return value_;}
    std::size_t workspace_bytes() const noexcept;
private:
    bool polar(std::span<const float>) noexcept;
    double distance(std::size_t) const noexcept;
    bool evaluate(float&) noexcept;
    void commit_history() noexcept;
    std::vector<double> magnitude_,phase_,previous_magnitude_,previous_phase_,older_phase_;
    double value_=0;
};
}
