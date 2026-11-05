// SPDX-License-Identifier: AGPL-3.0-or-later
#include "research_profile.h"
#include <rhythm_graph_adapters/profile.h>
#include <sinc_backend.h>
#include <algorithm>

namespace {
constexpr std::size_t research_elements=4194304,research_payload=16777216;
}

dataflow_runtime::NumericRuntimeProfile attach_research_operations(dataflow_runtime::NumericRuntimeProfile profile) {
    profile.kernels.max_elements=std::max(profile.kernels.max_elements,research_elements);
    profile.kernels.max_input_bytes=std::max(profile.kernels.max_input_bytes,research_payload);
    profile.kernels.max_output_bytes=std::max(profile.kernels.max_output_bytes,research_payload);
    return rhythm_graph_adapters::with_reference_operations(std::move(profile),{},
        std::shared_ptr<const dsp_kernels::ResamplerBackendFactory>(&desktop_resampling::sinc_backend_factory(),[](auto*){}));
}

dataflow_runtime::NumericRuntimeProfile research_profile(dataflow_runtime::NumericRuntimeProfile profile,
    std::shared_ptr<const dsp_kernels::FourierBackendFactory> fourier) {
    profile.admission.frame.max_elements=research_elements;profile.admission.frame.max_payload_bytes=research_payload;
    profile.resources.maximum.payload_bytes=536870912;
    profile.resources.maximum.scratch_bytes=536870912;
    profile.resources.max_peak_bytes=2147483648ULL;
    profile.fourier_backend=std::move(fourier);
    return attach_research_operations(std::move(profile));
}

dataflow_runtime::NumericRuntimeProfile offline_research_profile(
    std::shared_ptr<const dsp_kernels::FourierBackendFactory> fourier) {
    constexpr signal_contracts::FrameLimits frames{8,4194304,16777216,65536,4096,65536};
    dataflow_runtime::NumericRuntimeProfile profile{{512,2048,16,32,1048576,4194304,frames},
        // Input/collector, bounded captures and serialization/decoder ownership
        // are charged against the same process peak before graph preparation.
        {{33554432,536870912,536870912,1048576},335544320,2147483648ULL},
        {4194304,8,16777216,16777216,16777216},{true,true,true}};
    profile.music_enabled=true;profile.experimental_resampling_enabled=true;
    return research_profile(std::move(profile),std::move(fourier));
}
