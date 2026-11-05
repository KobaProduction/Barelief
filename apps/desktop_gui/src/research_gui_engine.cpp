// SPDX-License-Identifier: AGPL-3.0-or-later
#include "research_gui_engine.h"
#include "research_profile.h"
#include <kiss_backend.h>
#include <ooura_backend.h>

namespace {
dataflow_runtime::NumericRuntimeProfile gui_research_profile(dataflow_runtime::NumericRuntimeProfile profile) {
    profile.additional_fourier_backends.emplace_back(&desktop_fourier::kiss_backend_factory(),[](const auto*){});
    profile.additional_fourier_backends.emplace_back(&desktop_fourier::ooura_backend_factory(),[](const auto*){});
    return attach_research_operations(std::move(profile));
}
}

std::unique_ptr<pipeline::NumericEngine> make_research_gui_engine(pipeline::AudioSourceFactory source) {
    return std::make_unique<pipeline::NumericEngine>(std::move(source),nullptr,gui_research_profile);
}
