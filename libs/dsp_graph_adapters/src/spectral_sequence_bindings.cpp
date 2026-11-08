#include "spectral_sequence.h"
#include <algorithm>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
bool offset_output(NumericOperationOutput& output,std::ptrdiff_t value) noexcept {
    return set_shape(output,1) && store_element(output.value,{0,0,0},std::int64_t(value))==Status::ok;
}
bool novelty_diagnostics(const PreparedSpectralNovelty& source,std::span<NumericOperationOutput> output,domain::PortId first) noexcept {
    return write_values(*output_at(output,first),source.difference_values()) &&
        write_values(*output_at(output,domain::PortId(first+1)),source.phase_mask_values()) &&
        write_values(*output_at(output,domain::PortId(first+2)),source.alignment_correlations()) &&
        offset_output(*output_at(output,domain::PortId(first+3)),source.alignment_offset());
}
class Novelty final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Novelty>(*this);}
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n) {
        if(!spectral_sequence_plan(n,plan_))return false;
        supports_=own_spectral_supports(n);
        return kernel_.prepare(plan_.frames,plan_.features,plan_.bins,plan_.novelty,supports_);
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+supports_.capacity()*sizeof(FilterBandSupport);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& input=*input_at(in,0);const auto* phase=available_input_at(in,1);auto& target=*output_at(out,0);
        if(input.value.actual[1]!=plan_.features || (phase && (phase->value.actual[0]!=input.value.actual[0] || phase->value.actual[1]!=plan_.bins)))return false;
        return set_shape(target,input.value.actual[0],plan_.columns) &&
            kernel_.process(rows(input),phase?rows(*phase):FloatMatrixSource{},input.value.actual[0],rows(target)) &&
            novelty_diagnostics(kernel_,out,1);
    }
    SpectralSequencePlan plan_;PreparedSpectralNovelty kernel_;std::vector<FilterBandSupport> supports_;
};
}
PreparedSequence prepare_spectral_sequence(const dataflow_runtime::NumericOperationNode& node) {
    const auto n=definition(node);
    if(n.kind=="SpectralNovelty"){auto value=std::make_unique<Novelty>();return value->prepare(n)?std::move(value):nullptr;}
    return {};
}
domain::ProcessingSupport assess_spectral_sequence(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& profile) noexcept {
    SpectralSequencePlan plan;domain::ProcessingSupport support;
    if(!spectral_sequence_plan(n,plan) || !spectral_sequence_outputs(n,plan))return {};
    const auto object=sizeof(Novelty);
    if(!prepared_storage::add_array(plan.kernel_bytes,array(n,"support_first").size(),sizeof(FilterBandSupport)))return {};
    descriptor_resources(n,profile,object,plan.kernel_bytes,support);return support;
}
}
