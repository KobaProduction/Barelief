#pragma once
#include <dataflow_runtime/numeric_operation.h>
#include <dsp_kernels/gaussian_mixture.h>
#include <dsp_kernels/pattern_state_decoder.h>
#include <dsp_kernels/downbeat_state_decoder.h>
#include <dsp_kernels/bar_state_decoder.h>
#include <memory>
#include <string_view>

namespace dsp_graph_adapters {
// Classical immutable numerical parameters, supplied by the composition root.
// No file formats, neural models, fitting, JSON or pickle are processed here.
struct ClassicalModel:public dataflow_runtime::NumericOperationResources {
    dsp_kernels::GaussianMixtureData mixture;
    std::vector<dsp_kernels::GmmPatternData> patterns;
    std::vector<dsp_kernels::PatternTempoParameters> tempi;
    std::vector<dsp_kernels::DownbeatPatternConfig> downbeat_patterns;
    std::vector<std::uint32_t> meters;
    std::vector<float> pattern_switches;
    bool owned_bytes(std::size_t&) const noexcept override;
};
class ClassicalModelProvider {
public:
    virtual ~ClassicalModelProvider()=default;
    // Pure lookup. Returned storage is immutable, owned, and pinned by plans.
    virtual std::shared_ptr<const ClassicalModel> find(std::string_view key) const noexcept=0;
};
}
