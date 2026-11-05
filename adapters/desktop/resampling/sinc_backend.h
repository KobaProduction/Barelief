#pragma once
#include <dsp_kernels/prepared_resampler.h>
namespace desktop_resampling {
// Full SRC_SINC_BEST_QUALITY table, independently reset finite-window mode.
// This factory supplies prepare_window/query_window; streaming is unsupported.
const dsp_kernels::ResamplerBackendFactory& sinc_backend_factory() noexcept;
}
