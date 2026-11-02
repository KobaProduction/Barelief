// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <dsp_kernels/activation_peaks.h>
#include <dsp_kernels/numeric_rows.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace dsp_kernels {
struct ActivationPeakPosition {std::size_t frame=0,channel=0;};
// Source rank2 peak_picking: independent time filtering, input-sized same
// convolution even for a single channel, then row-major nonzero coordinates.
// The reader must be stable across channel passes. Only one row/column and
// masks are staged; the input matrix is never cloned. Output storage is owned
// until the next process call. Failure invalidates the published result.
// Empty matrix dimensions yield an empty result. All storage is prepared cold.
class PreparedActivationPeakMatrix {
public:
    static bool storage_bytes(std::size_t frames,std::size_t channels,ActivationPeakConfig,
        std::span<const float> smoothing,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,std::size_t maximum_channels,
        ActivationPeakConfig={},std::span<const float> smoothing_kernel={});
    bool process(FloatMatrixSource,std::size_t frames,std::size_t channels) noexcept;
    std::span<const ActivationPeakPosition> positions() const noexcept {return std::span(positions_).first(count_);}
private:
    bool read_column(FloatMatrixSource,std::size_t frames,std::size_t channels,std::size_t channel) noexcept;
    bool process_channel(FloatMatrixSource,std::size_t frames,std::size_t channels,
        std::size_t channel,std::size_t& count) noexcept;
    PreparedActivationPeaks peaks_;
    std::size_t maximum_frames_=0,maximum_channels_=0,count_=0;
    std::vector<float> row_,column_;
    std::vector<std::uint8_t> mask_;
    std::vector<ActivationPeakPosition> positions_;
};
}
