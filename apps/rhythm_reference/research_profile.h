// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/runtime.h>

dataflow_runtime::NumericRuntimeProfile attach_research_operations(dataflow_runtime::NumericRuntimeProfile);

dataflow_runtime::NumericRuntimeProfile research_profile(
    dataflow_runtime::NumericRuntimeProfile,
    std::shared_ptr<const dsp_kernels::FourierBackendFactory>);
// Bounded, optional offline composition; no desktop capture/transport owner.
dataflow_runtime::NumericRuntimeProfile offline_research_profile(
    std::shared_ptr<const dsp_kernels::FourierBackendFactory>);
