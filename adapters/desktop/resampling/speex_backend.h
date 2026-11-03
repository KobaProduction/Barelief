#pragma once
#include <dsp_kernels/prepared_resampler.h>
namespace desktop_resampling {
// Desktop composition installs this factory; portable/ESP compositions omit it.
const dsp_kernels::ResamplerBackendFactory& speex_backend_factory() noexcept;
bool query_speex_q15(const dsp_kernels::ResamplerConfig&,dsp_kernels::ResamplerResources&) noexcept;
std::unique_ptr<dsp_kernels::ResamplerBackend> prepare_speex_q15(const dsp_kernels::ResamplerConfig&);
}
