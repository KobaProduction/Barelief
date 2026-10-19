#pragma once
#include <dsp_kernels/prepared_resampler.h>
namespace desktop_resampling {
// Desktop composition installs this factory; portable/ESP compositions omit it.
const dsp_kernels::ResamplerBackendFactory& speex_backend_factory() noexcept;
}
