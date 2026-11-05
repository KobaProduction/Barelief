#include "sequence_settings.h"
#include <dsp_kernels/beat_tracking_sequence.h>
#include <dsp_kernels/frame_timing.h>
#include <algorithm>
#include <cmath>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
bool tracking_config(const domain::ResolvedProcessingNode& n,BeatTrackingSequenceConfig& c) noexcept {
    return read_histogram_settings(n,c.histogram) && number(n,"activation_smoothing_seconds",.14f,c.activation_smoothing_seconds) &&
        number(n,"look_aside",.2f,c.look_aside) && number(n,"look_ahead_seconds",10,c.look_ahead_seconds) &&
        integer(n,"histogram_smoothing",9,c.histogram_smoothing) && flag(n,"variable_tempo",true,c.variable_tempo);
}
bool crf_config(const domain::ResolvedProcessingNode& n,CrfBeatSequenceConfig& c) noexcept {
    return read_analysis_settings(n,c.tempo) && number(n,"interval_sigma",.18f,c.interval_sigma) &&
        flag(n,"use_factors",false,c.use_factors) && integer(n,"num_candidates",5,c.num_intervals);
}
std::span<const float> factors(const domain::ResolvedProcessingNode& n) noexcept {
    return parameter(n,"factors")?array(n,"factors"):std::span<const float>(default_beat_interval_factors);
}
bool histogram_geometry(const TempoHistogramSequenceConfig& c,IntervalHistogramConfig& h) noexcept {
    return interval_histogram_parameters(c.frame_rate,c.minimum_bpm,c.maximum_bpm,c.history_seconds,h);
}
std::size_t candidate_count(const IntervalHistogramConfig& h,bool interpolate) noexcept {
    if(!interpolate)return h.maximum_interval-h.minimum_interval+1;
    const float count=std::ceil((float(h.maximum_interval)-float(h.minimum_interval))*100.f);
    return std::isfinite(count) && count>=1 && count<float(SIZE_MAX)?std::size_t(count):0;
}
bool write_candidates(NumericOperationOutput& o,std::span<const HistogramTempo> values) noexcept {
    if(!set_shape(o,values.size(),2))return false;
    for(std::size_t k=0;k<values.size();++k)
        if(store_element(o.value,{0,k,0},values[k].tempo)!=Status::ok || store_element(o.value,{0,k,1},values[k].strength)!=Status::ok)return false;
    return true;
}
class Histogram final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Histogram>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;TempoHistogramSequenceConfig c;
        const auto input=contract(n,domain::PortDirection::Input,0);if(!input)return false;
        return input_geometry(n,0,input->shape.rank,a,b) && read_histogram_settings(n,c) &&
            flag(n,"reset_state",true,reset_) && c.channels==b && kernel_.prepare(a,c,array(n,"comb_alpha"));
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& input=*input_at(in,0);const auto values=input.contiguous_float32;
        if(values.size()/kernel_.channels()!=input.value.actual[0] || input.value.actual[1]!=kernel_.channels())return false;
        return kernel_.process(values,reset_) && write_values(*output_at(out,0),kernel_.bins()) && write_values(*output_at(out,1),kernel_.coordinates());
    }
    PreparedTempoHistogramSequence kernel_;bool reset_=true;
};
class Analysis final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Analysis>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;TempoAnalysisSequenceConfig c;
        return input_geometry(n,0,Rank::Vector,a,b) && read_analysis_settings(n,c) && flag(n,"reset_state",true,reset_) &&
            kernel_.prepare(a,c,array(n,"activation_kernel"),array(n,"histogram_kernel"),array(n,"comb_alpha"));
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        return kernel_.process(input_at(in,0)->contiguous_float32,reset_) && write_candidates(*output_at(out,0),kernel_.candidates()) &&
            write_values(*output_at(out,1),kernel_.raw_histogram()) && write_values(*output_at(out,2),kernel_.histogram_coordinates()) &&
            write_values(*output_at(out,3),kernel_.smoothed_histogram());
    }
    PreparedTempoAnalysisSequence kernel_;bool reset_=true;
};
class BeatTracking final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<BeatTracking>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;BeatTrackingSequenceConfig c;
        return input_geometry(n,0,Rank::Vector,a,b) && tracking_config(n,c) && kernel_.prepare(a,c);
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        return kernel_.process(input_at(in,0)->contiguous_float32) && write_values(*output_at(out,0),kernel_.events()) &&
            write_indices(*output_at(out,1),kernel_.positions()) && write_values(*output_at(out,2),kernel_.histogram());
    }
    PreparedBeatTrackingSequence kernel_;
};
class Crf final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Crf>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;CrfBeatSequenceConfig c;
        return input_geometry(n,0,Rank::Vector,a,b) && crf_config(n,c) && kernel_.prepare(a,c,factors(n));
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        return kernel_.process(input_at(in,0)->contiguous_float32) && write_values(*output_at(out,0),kernel_.events()) &&
            write_indices(*output_at(out,1),kernel_.positions()) && write_indices(*output_at(out,2),kernel_.trial_intervals()) &&
            write_values(*output_at(out,3),kernel_.normalized_scores()) && write_candidates(*output_at(out,4),kernel_.tempo_candidates());
    }
    PreparedCrfBeatSequence kernel_;
};
domain::ProcessingSupport histogram_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0,total=0;TempoHistogramSequenceConfig c;IntervalHistogramConfig h;domain::ProcessingSupport s;
    const auto input=contract(n,domain::PortDirection::Input,0);
    if(!input || (input->shape.rank!=Rank::Vector && input->shape.rank!=Rank::Matrix) || !input_geometry(n,0,input->shape.rank,a,b) ||
       !read_histogram_settings(n,c) || c.channels!=b || !histogram_geometry(c,h) || !PreparedTempoHistogramSequence::storage_bytes(a,c,array(n,"comb_alpha"),bytes) ||
       !prepared_storage::mul_bytes(h.maximum_interval-h.minimum_interval+1,b,total) || !output_geometry(n,0,Rank::Vector,total) ||
       !output_geometry(n,1,Rank::Vector,h.maximum_interval-h.minimum_interval+1))return {};
    descriptor_resources(n,p,sizeof(Histogram),bytes,s);return s;
}
domain::ProcessingSupport analysis_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0;TempoAnalysisSequenceConfig c;IntervalHistogramConfig h;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !read_analysis_settings(n,c) || !histogram_geometry(c.histogram,h) ||
       !PreparedTempoAnalysisSequence::storage_bytes(a,c,array(n,"activation_kernel"),array(n,"histogram_kernel"),array(n,"comb_alpha"),bytes))return {};
    const auto bins=h.maximum_interval-h.minimum_interval+1,peaks=candidate_count(h,c.interpolate);
    if(!peaks || !output_geometry(n,0,Rank::Matrix,peaks,2) || !output_geometry(n,1,Rank::Vector,bins) ||
       !output_geometry(n,2,Rank::Vector,bins) || !output_geometry(n,3,Rank::Vector,bins))return {};
    descriptor_resources(n,p,sizeof(Analysis),bytes,s);return s;
}
domain::ProcessingSupport tracking_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0,smoothing=0;BeatTrackingSequenceConfig c;IntervalHistogramConfig h;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !tracking_config(n,c) || !histogram_geometry(c.histogram,h) ||
       !truncated_frame_count(c.activation_smoothing_seconds,c.histogram.frame_rate,smoothing) || !PreparedBeatTrackingSequence::storage_bytes(a,c,bytes))return {};
    const auto count=std::max(a,smoothing)+1;
    if(!output_geometry(n,0,Rank::Vector,count) || !output_geometry(n,1,Rank::Vector,count,1,DType::UInt64) ||
       !output_geometry(n,2,Rank::Vector,h.maximum_interval-h.minimum_interval+1))return {};
    descriptor_resources(n,p,sizeof(BeatTracking),bytes,s);return s;
}
domain::ProcessingSupport crf_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0,smoothing=0;CrfBeatSequenceConfig c;IntervalHistogramConfig h;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !crf_config(n,c) || !histogram_geometry(c.tempo.histogram,h) ||
       !truncated_frame_count(c.tempo.activation_smoothing_seconds,c.tempo.histogram.frame_rate,smoothing) || !PreparedCrfBeatSequence::storage_bytes(a,c,factors(n),bytes))return {};
    const auto count=std::max<std::size_t>(1,std::max(a,smoothing)/h.minimum_interval),trials=c.use_factors?factors(n).size():c.num_intervals,peaks=candidate_count(h,c.tempo.interpolate);
    if(!peaks || !output_geometry(n,0,Rank::Vector,count) || !output_geometry(n,1,Rank::Vector,count,1,DType::UInt64) ||
       !output_geometry(n,2,Rank::Vector,trials,1,DType::UInt64) || !output_geometry(n,3,Rank::Vector,trials) || !output_geometry(n,4,Rank::Matrix,peaks,2))return {};
    descriptor_resources(n,p,sizeof(Crf),bytes,s);return s;
}
}
PreparedSequence prepare_histogram(const domain::ConfiguredProcessingNode& node) {
    const auto n=definition(node);
    if(n.kind=="TempoHistogram"){auto op=std::make_unique<Histogram>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="TempoAnalysis"){auto op=std::make_unique<Analysis>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="BeatTrackingSequence"){auto op=std::make_unique<BeatTracking>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="CrfBeatSequence"){auto op=std::make_unique<Crf>();return op->prepare(n)?std::move(op):nullptr;}
    return {};
}
domain::ProcessingSupport assess_histogram(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    if(n.kind=="TempoHistogram")return histogram_support(n,p);
    if(n.kind=="TempoAnalysis")return analysis_support(n,p);
    if(n.kind=="BeatTrackingSequence")return tracking_support(n,p);
    if(n.kind=="CrfBeatSequence")return crf_support(n,p);
    return {};
}
}
