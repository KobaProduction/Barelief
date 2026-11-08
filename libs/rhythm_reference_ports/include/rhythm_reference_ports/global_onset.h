// SPDX-License-Identifier: AGPL-3.0-or-later
// Essentia source ports; see ../AGPL-COPYING and implementation provenance.
#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
// Input is one magnitude spectrum, with the explicit source bin interval.
// Zero-initialized eleven-frame history; output describes the frame five
// frames behind the newest input. The source does not flush a padded tail.
class PreparedInformationGain {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t bins,std::size_t first_bin,std::size_t stop_bin,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t bins,std::size_t first_bin,std::size_t stop_bin);
    void reset() noexcept;
    bool process(std::span<const float> magnitude,float& detection) noexcept;
private:
    float bin_change(std::size_t) const noexcept;
    std::size_t bins_=0,first_=0,count_=0,next_=0;
    std::vector<float> history_;
};
}
