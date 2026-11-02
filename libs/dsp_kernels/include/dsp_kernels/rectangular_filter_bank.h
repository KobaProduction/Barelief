#pragma once
#include <dsp_kernels/mel_filter_bank.h>
#include <span>

namespace dsp_kernels {
// madmom RectangularFilterbank: nearest-bin borders, upper bin on midpoint ties,
// half-open supports, optional unique borders and normalization. Source output
// band count is borders-1 EVEN when unique corners collapse (trailing zero bands).
// Uses existing MelFilterBank's explicit matrix projection; no frequency-grid
// inference and no magnitude squaring. Coordinates are caller supplied.
class PreparedRectangularFilterBank {
public:
    // Owned dynamic cold peak, including temporary corners/weights/supports.
    static bool storage_bytes(std::size_t bins,std::size_t border_count,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return projection_.workspace_bytes();}
    bool prepare(std::span<const float> bin_frequencies,std::span<const float> borders,
                 bool normalize=true,bool unique_borders=true);
    bool process(std::span<const float> values,std::span<float> output) const noexcept {return projection_.process(values,output);}
    std::size_t bands() const noexcept {return bands_;}
private:
    std::size_t bands_=0;
    MelFilterBank projection_;
};
}
