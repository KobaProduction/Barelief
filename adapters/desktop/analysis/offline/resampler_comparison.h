#pragma once
#include <dsp_kernels/prepared_resampler.h>
#include <atomic>
#include <vector>

namespace desktop_analysis::offline {
struct ComparisonRequest {
    std::uint32_t input_rate=48000, output_rate=32768;
    unsigned quality=5;
    std::size_t block_samples=480, blocks=200;
};
struct ComparisonResult {
    dsp_kernels::ResamplerMode mode{};
    std::uint32_t rate=0;
    double prepare=0, p50=0, p95=0;
    dsp_kernels::ResamplerResources resources;
    bool valid=false;
};
// Silent, bounded synthetic processing. No widgets, threads or audio devices.
std::vector<ComparisonResult> compare_resamplers(const ComparisonRequest&, const std::atomic_bool&,
    const dsp_kernels::ResamplerBackendFactory* external=nullptr);
}
