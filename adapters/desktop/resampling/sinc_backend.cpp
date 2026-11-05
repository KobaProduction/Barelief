// SPDX-License-Identifier: BSD-2-Clause
#include "sinc_backend.h"
#include <dsp_kernels/sinc_resampling_window.h>
#include <iterator>
namespace desktop_resampling {
namespace {
using coeff_t=float;
#include "vendor/libsamplerate/high_qual_coeffs.h"
dsp_kernels::SincCoefficientTable coefficient_table() noexcept {
    return {slow_high_qual_coeffs.coeffs,std::uint32_t(slow_high_qual_coeffs.increment)};
}
class SincBackend final:public dsp_kernels::ResamplerBackend {
public:
    bool prepare(dsp_kernels::ResamplingWindowConfig c) {
        if(!window_.prepare(c,coefficient_table()))return false;
        config_=c;return true;
    }
    bool process(std::span<const float>,std::size_t,std::size_t,
        std::span<float>,std::size_t,std::size_t& written) noexcept override {written=0;return false;}
    bool process_window(std::span<const float> input,std::size_t stride,
        std::span<float> output,std::size_t capacity,std::size_t& written) noexcept override {
        return window_.process(input,stride,output,capacity,written);
    }
    void reset() noexcept override {}
    bool copy_state_from(const dsp_kernels::ResamplerBackend& source) noexcept override {
        const auto* other=dynamic_cast<const SincBackend*>(&source);
        return other && other->config_==config_;
    }
    dsp_kernels::ResamplerResources resources() const noexcept override {
        dsp_kernels::ResamplerResources result;
        result.state_bytes=window_.owned_bytes();result.owned_bytes=sizeof(*this)+result.state_bytes;
        return result;
    }
private:
    dsp_kernels::ResamplingWindowConfig config_;
    dsp_kernels::PreparedSincResamplingWindow window_;
};
class SincFactory final:public dsp_kernels::ResamplerBackendFactory {
public:
    std::unique_ptr<dsp_kernels::ResamplerBackend> prepare(const dsp_kernels::ResamplerConfig&) const override {return {};}
    std::unique_ptr<dsp_kernels::ResamplerBackend> prepare_window(const dsp_kernels::ResamplingWindowConfig& c) const override {
        if(c.mode!=dsp_kernels::ResamplingWindowMode::SincBestQuality)return {};
        auto backend=std::make_unique<SincBackend>();
        if(!backend->prepare(c))return {};
        return backend;
    }
    bool query_window(const dsp_kernels::ResamplingWindowConfig& c,dsp_kernels::ResamplerResources& r) const noexcept override {
        r={};
        if(c.mode!=dsp_kernels::ResamplingWindowMode::SincBestQuality)return false;
        if(!dsp_kernels::PreparedSincResamplingWindow::storage_bytes(c,r.state_bytes))return false;
        if(r.state_bytes>SIZE_MAX-sizeof(SincBackend))return false;
        r.owned_bytes=sizeof(SincBackend)+r.state_bytes;return true;
    }
};
}
const dsp_kernels::ResamplerBackendFactory& sinc_backend_factory() noexcept {
    static const SincFactory factory;return factory;
}
}
