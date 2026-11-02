#include "sequence_settings.h"
#include <dsp_kernels/metrical_tempogram_sequence.h>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
std::optional<std::span<const float>> factors(const domain::ResolvedProcessingNode& n) noexcept {
    return parameter(n,"factors")?std::optional(array(n,"factors")):std::nullopt;
}
bool dynamic_contract(const domain::ResolvedProcessingNode& n,domain::PortId port,std::size_t a,std::size_t b,bool required) noexcept {
    if(!contract(n,domain::PortDirection::Input,port))return !required;
    std::size_t x=0,y=0;return input_geometry(n,port,Rank::Matrix,x,y) && x==a && y==b;
}
bool prior_contract(const domain::ResolvedProcessingNode& n,std::size_t bins) noexcept {
    if(!contract(n,domain::PortDirection::Input,2))return true;
    std::size_t a=0,b=0;return input_geometry(n,2,Rank::Vector,a,b) && a==bins;
}
bool bpm_contract(const domain::ResolvedProcessingNode& n,std::size_t frames) noexcept {
    if(!contract(n,domain::PortDirection::Input,1))return true;
    std::size_t a=0,b=0;return input_geometry(n,1,Rank::Vector,a,b) && (a==1 || a==frames);
}
class Ratio final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Ratio>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;TempoRatioSequenceConfig c;
        return input_geometry(n,0,Rank::Matrix,a,b) && read_ratio_settings(n,c) && kernel_.prepare(a,b,c,array(n,"coordinates"),factors(n));
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& v=*input_at(in,0);const auto* bpm=available_input_at(in,1);const auto* prior=available_input_at(in,2);const auto* frequency=available_input_at(in,3);auto& target=*output_at(out,0);
        if(frequency && frequency->value.actual!=v.value.actual)return false;
        return set_shape(target,kernel_.output_frames(v.value.actual[0]),kernel_.factors()) && kernel_.process(rows(v),v.value.actual[0],rows(target),
            bpm?std::optional(bpm->contiguous_float32):std::nullopt,prior?prior->contiguous_float32:std::span<const float>{},frequency?rows(*frequency):FloatMatrixSource{});
    }
    PreparedTempoRatioSequence kernel_;
};
class Metrical final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Metrical>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        TempoRatioSequenceConfig settings;if(!read_ratio_settings(n,settings,FeatureReduction::Sum))return false;
        MetricalTempogramSequenceConfig c;c.interpolation=settings.interpolation;c.tempo_reduction=settings.time_reduction;c.dynamic_frequencies=settings.dynamic_frequencies;
        return kernel_.prepare(array(n,"coordinates"),c,factors(n));
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& v=*input_at(in,0);const auto* frequency=available_input_at(in,1);auto& target=*output_at(out,0);
        if(frequency && frequency->value.actual!=v.value.actual)return false;
        return set_shape(target,v.value.actual[0],kernel_.columns()) && kernel_.process(rows(v),v.value.actual[0],rows(target),frequency?rows(*frequency):FloatMatrixSource{});
    }
    PreparedMetricalTempogramSequence kernel_;
};
class EnvelopeRatios final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<EnvelopeRatios>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;EnvelopeTempoRatiosConfig c;
        return input_geometry(n,0,Rank::Vector,a,b) && read_envelope_ratio_settings(n,c) &&
            kernel_.prepare(a,c,array(n,"coefficients"),array(n,"coordinates"),factors(n));
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& v=*input_at(in,0);const auto* bpm=available_input_at(in,1);const auto* prior=available_input_at(in,2);const auto* frequency=available_input_at(in,3);auto& target=*output_at(out,0);
        return set_shape(target,kernel_.output_frames(v.value.actual[0]),kernel_.factors()) && kernel_.process(v.contiguous_float32,rows(target),
            bpm?std::optional(bpm->contiguous_float32):std::nullopt,prior?prior->contiguous_float32:std::span<const float>{},frequency?rows(*frequency):FloatMatrixSource{});
    }
    PreparedEnvelopeTempoRatios kernel_;
};
domain::ProcessingSupport ratio_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0;TempoRatioSequenceConfig c;domain::ProcessingSupport s;
    const auto f=factors(n);const auto count=f?f->size():13;const auto grid=array(n,"coordinates");
    if(!input_geometry(n,0,Rank::Matrix,a,b) || !read_ratio_settings(n,c) || (!grid.empty() && grid.size()!=b) ||
       !bpm_contract(n,a) || !prior_contract(n,b) || !dynamic_contract(n,3,a,b,c.dynamic_frequencies) ||
       !PreparedTempoRatioSequence::storage_bytes(a,b,c,count,bytes) || !output_geometry(n,0,Rank::Matrix,c.time_reduction.operation==FeatureReduction::None?a:1,count))return {};
    descriptor_resources(n,p,sizeof(Ratio),bytes,s);return s;
}
domain::ProcessingSupport metrical_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0,columns=0;TempoRatioSequenceConfig settings;domain::ProcessingSupport s;
    const auto f=factors(n);const auto count=f?f->size():4;const auto grid=array(n,"coordinates");
    if(!input_geometry(n,0,Rank::Matrix,a,b) || !read_ratio_settings(n,settings,FeatureReduction::Sum) || grid.size()!=b || !dynamic_contract(n,1,a,b,settings.dynamic_frequencies))return {};
    MetricalTempogramSequenceConfig c;c.interpolation=settings.interpolation;c.tempo_reduction=settings.time_reduction;c.dynamic_frequencies=settings.dynamic_frequencies;
    columns=count;if(c.tempo_reduction.operation==FeatureReduction::None && !prepared_storage::mul_bytes(count,b,columns))return {};
    if(!PreparedMetricalTempogramSequence::storage_bytes(b,c,count,bytes) || !output_geometry(n,0,Rank::Matrix,a,columns))return {};
    descriptor_resources(n,p,sizeof(Metrical),bytes,s);return s;
}
domain::ProcessingSupport envelope_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0,frames=0;EnvelopeTempoRatiosConfig c;domain::ProcessingSupport s;
    const auto f=factors(n);const auto count=f?f->size():13;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !read_envelope_ratio_settings(n,c) || !PreparedTempogramSequence::frame_count(c.tempogram,a,frames) ||
       !bpm_contract(n,frames) || !prior_contract(n,c.tempogram.autocorrelation.window.length) ||
       !dynamic_contract(n,3,frames,c.tempogram.autocorrelation.window.length,c.ratios.dynamic_frequencies) ||
       !PreparedEnvelopeTempoRatios::storage_bytes(a,c,array(n,"coefficients"),array(n,"coordinates"),f,bytes) ||
       !output_geometry(n,0,Rank::Matrix,c.ratios.time_reduction.operation==FeatureReduction::None?frames:1,count))return {};
    descriptor_resources(n,p,sizeof(EnvelopeRatios),bytes,s);return s;
}
}
PreparedSequence prepare_tempo_ratio(const domain::ConfiguredProcessingNode& node) {
    const auto n=definition(node);
    if(n.kind=="TempoRatios"){auto op=std::make_unique<Ratio>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="MetricalTempogram"){auto op=std::make_unique<Metrical>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="EnvelopeTempoRatios"){auto op=std::make_unique<EnvelopeRatios>();return op->prepare(n)?std::move(op):nullptr;}
    return {};
}
domain::ProcessingSupport assess_tempo_ratio(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    if(n.kind=="TempoRatios")return ratio_support(n,p);
    if(n.kind=="MetricalTempogram")return metrical_support(n,p);
    if(n.kind=="EnvelopeTempoRatios")return envelope_support(n,p);
    return {};
}
}
