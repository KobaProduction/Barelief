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
    bool prepare(const domain::ResolvedProcessingNode& n) {
        if(!spectral_sequence_plan(n,plan_))return false;
        supports_=own_spectral_supports(n);
        return kernel_.prepare(plan_.frames,plan_.features,plan_.bins,plan_.pcm.novelty,supports_);
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
class PcmNovelty final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<PcmNovelty>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        if(!spectral_sequence_plan(n,plan_))return false;
        const auto centers=array(n,"centers_hz");centers_.assign(centers.begin(),centers.end());
        auto filters=plan_.filters;filters.centers_hz=centers_;
        if(!kernel_.prepare(plan_.samples,plan_.pcm,array(n,"coefficients"),filters))return false;
        plan_.filters={};return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+centers_.capacity()*sizeof(double);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto pcm=input_at(in,0)->contiguous_float32;auto& target=*output_at(out,0);
        if(!set_shape(target,kernel_.frames(pcm.size()),plan_.columns) || !kernel_.process(pcm,rows(target)))return false;
        const auto view=kernel_.spectrum().values();auto& complex=*output_at(out,1);
        if(!set_shape(complex,view.complex.size()/2,2))return false;
        for(std::size_t k=0;k<view.complex.size();++k)
            if(store_element(complex.value,{0,k/2,k%2},view.complex[k])!=Status::ok)return false;
        return write_values(*output_at(out,2),view.magnitude) && write_values(*output_at(out,3),view.phase) &&
            write_values(*output_at(out,4),view.features) && novelty_diagnostics(kernel_.novelty(),out,5);
    }
    SpectralSequencePlan plan_;PreparedPcmSpectralNovelty kernel_;std::vector<double> centers_;
};
class PeakMatrix final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<PeakMatrix>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        SpectralSequencePlan p;return spectral_sequence_plan(n,p) && kernel_.prepare(p.frames,p.features,p.peaks,array(n,"smoothing"));
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& input=*input_at(in,0);
        if(!kernel_.process(rows(input),input.value.actual[0],input.value.actual[1]))return false;
        const auto positions=kernel_.positions();auto& frames=*output_at(out,0);auto& channels=*output_at(out,1);
        if(!set_shape(frames,positions.size()) || !set_shape(channels,positions.size()))return false;
        for(std::size_t i=0;i<positions.size();++i)
            if(store_element(frames.value,{0,i,0},std::uint64_t(positions[i].frame))!=Status::ok ||
               store_element(channels.value,{0,i,0},std::uint64_t(positions[i].channel))!=Status::ok)return false;
        return true;
    }
    PreparedActivationPeakMatrix kernel_;
};
}
PreparedSequence prepare_spectral_sequence(const domain::ConfiguredProcessingNode& node) {
    const auto n=definition(node);
    if(n.kind=="SpectralNovelty"){auto value=std::make_unique<Novelty>();return value->prepare(n)?std::move(value):nullptr;}
    if(n.kind=="PcmSpectralNovelty"){auto value=std::make_unique<PcmNovelty>();return value->prepare(n)?std::move(value):nullptr;}
    if(n.kind=="ActivationPeakMatrix"){auto value=std::make_unique<PeakMatrix>();return value->prepare(n)?std::move(value):nullptr;}
    return {};
}
domain::ProcessingSupport assess_spectral_sequence(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& profile) noexcept {
    SpectralSequencePlan plan;domain::ProcessingSupport support;
    if(!spectral_sequence_plan(n,plan) || !spectral_sequence_outputs(n,plan))return {};
    const auto object=plan.matrix_peaks?sizeof(PeakMatrix):plan.pcm_input?sizeof(PcmNovelty):sizeof(Novelty);
    if(!prepared_storage::add_array(plan.kernel_bytes,array(n,"support_first").size(),sizeof(FilterBandSupport)) ||
       !prepared_storage::add_array(plan.kernel_bytes,array(n,"centers_hz").size(),sizeof(double)))return {};
    descriptor_resources(n,profile,object,plan.kernel_bytes,support);return support;
}
}
