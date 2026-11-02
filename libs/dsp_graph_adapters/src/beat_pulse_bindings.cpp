#include "operation_support.h"
#include "sequence_settings.h"
#include <dsp_kernels/beat_tracker.h>
#include <dsp_kernels/envelope_beat_tracking.h>
#include <dsp_kernels/envelope_local_pulse.h>
#include <algorithm>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
bool tracking_config(const domain::ResolvedProcessingNode& n,BeatTrackerConfig& c) noexcept {
    return feature_rate(n,44100.f/512,c.frame_rate) && number(n,"tightness",100,c.tightness) &&
        integer(n,"maximum_period_frames",2048,c.maximum_period_frames) && flag(n,"trim",true,c.trim);
}
bool pulse_config(const domain::ResolvedProcessingNode& n,LocalPulseConfig& c) noexcept {
    return number(n,"frame_rate",44100.f/512,c.frame_rate) && integer(n,"window_length",384,c.window.length) &&
        integer(n,"hop",1,c.window.hop) && flag(n,"center",true,c.window.center) &&
        number(n,"minimum_bpm",30,c.min_bpm) && number(n,"maximum_bpm",300,c.max_bpm);
}
bool envelope_pulse_config(const domain::ResolvedProcessingNode& n,EnvelopeLocalPulseConfig& c) noexcept {
    return pulse_config(n,c.pulse) && number(n,"sample_rate",44100,c.sample_rate) && integer(n,"hop_samples",512,c.hop_samples);
}
bool publish_tracking(const PreparedBeatTracker& tracker,std::span<NumericOperationOutput> out,domain::PortId first) noexcept {
    auto& links=*output_at(out,domain::PortId(first+2));const auto values=tracker.backlinks();
    if(!write_values(*output_at(out,first),tracker.local_scores()) || !write_values(*output_at(out,domain::PortId(first+1)),tracker.cumulative_scores()) ||
       !set_shape(links,values.size()))return false;
    for(std::size_t k=0;k<values.size();++k)if(store_element(links.value,{0,k,0},values[k])!=Status::ok)return false;
    return true;
}
class BeatPath final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<BeatPath>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;BeatTrackerConfig c;
        if(!input_geometry(n,0,Rank::Vector,a,b) || !tracking_config(n,c) || !kernel_.prepare(a,c))return false;
        mask_.resize(a);return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+mask_.capacity();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto envelope=input_at(in,0)->contiguous_float32,bpm=input_at(in,1)->contiguous_float32;
        auto mask=std::span(mask_).first(envelope.size());
        const bool silent=bpm.size()==1 && bpm[0]==0 && std::all_of(envelope.begin(),envelope.end(),[](float v){return v==0;});
        const bool ready=silent?kernel_.silence(envelope.size(),mask):kernel_.process(envelope,bpm,mask);
        return ready && write_mask(*output_at(out,0),mask) && publish_tracking(kernel_,out,1);
    }
    PreparedBeatTracker kernel_;std::vector<std::uint8_t> mask_;
};
class EnvelopeBeatPath final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<EnvelopeBeatPath>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;EnvelopeBeatTrackingConfig c;
        return input_geometry(n,0,Rank::Vector,a,b) && read_tracking_settings(n,c) && kernel_.prepare(a,c);
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto envelope=input_at(in,0)->contiguous_float32;const auto* bpm=available_input_at(in,1);const auto* prior=available_input_at(in,2);BeatTrackingSequenceResult result;
        if(!kernel_.process(envelope,result,bpm?std::optional(bpm->contiguous_float32):std::nullopt,prior?prior->contiguous_float32:std::span<const float>{}))return false;
        auto& events=*output_at(out,0);bool written=false;
        if(events.value.contract->dtype==DType::UInt8)written=write_mask(events,result.dense);
        else if(events.value.contract->dtype==DType::Float64)written=write_seconds(events,result.seconds);
        else written=result.samples.empty()?write_indices(events,result.frames):write_samples(events,result.samples);
        return written && write_values(*output_at(out,1),result.bpm) && publish_tracking(kernel_.tracker(),out,2);
    }
    PreparedEnvelopeBeatTracking kernel_;
};
class EnvelopePulse final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<EnvelopePulse>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t a=0,b=0;EnvelopeLocalPulseConfig c;
        if(!input_geometry(n,0,Rank::Vector,a,b) || !envelope_pulse_config(n,c) || !kernel_.prepare(a,c,array(n,"coefficients")))return false;
        output_.resize(a);return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+output_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto envelope=input_at(in,0)->contiguous_float32;const auto* prior=available_input_at(in,1);auto result=std::span(output_).first(envelope.size());
        return kernel_.process(envelope,result,prior?prior->contiguous_float32:std::span<const float>{}) &&
            write_values(*output_at(out,0),result) && write_values(*output_at(out,1),kernel_.coordinates_bpm());
    }
    PreparedEnvelopeLocalPulse kernel_;std::vector<float> output_;
};
class Pulse final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<Pulse>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        LocalPulseConfig c;std::size_t maximum=0;
        if(!pulse_config(n,c) || !integer(n,"samples",512,maximum) || !kernel_.prepare(maximum,c,array(n,"coefficients")))return false;
        output_.resize(maximum);bins_=c.window.length/2+1;return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+output_.capacity()*sizeof(float);}
private:
    struct ComplexRows {const NumericOperationInput* real;const NumericOperationInput* imaginary;};
    static bool read(const void* context,std::size_t row,std::span<float> output) noexcept {
        const auto& source=*static_cast<const ComplexRows*>(context);
        if(output.size()!=2*source.real->value.actual[1])return false;
        for(std::size_t bin=0;bin<output.size()/2;++bin)
            if(load_element(source.real->value,{0,row,bin},output[2*bin])!=Status::ok ||
               load_element(source.imaginary->value,{0,row,bin},output[2*bin+1])!=Status::ok)return false;
        return true;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto* re=input_at(in,0);const auto* im=input_at(in,1);const auto* prior=available_input_at(in,2);
        if(!im || re->value.actual!=im->value.actual || re->value.actual[1]!=bins_)return false;
        const ComplexRows source{re,im};
        return kernel_.process({&source,read},re->value.actual[0],output_,prior?prior->contiguous_float32:std::span<const float>{}) && write_values(*output_at(out,0),output_);
    }
    PreparedLocalPulse kernel_;std::vector<float> output_;std::size_t bins_=0;
};
bool tracking_outputs(const domain::ResolvedProcessingNode& n,std::size_t frames,domain::PortId first) noexcept {
    return output_geometry(n,first,Rank::Vector,frames) && output_geometry(n,domain::PortId(first+1),Rank::Vector,frames) &&
        output_geometry(n,domain::PortId(first+2),Rank::Vector,frames,1,DType::Int32);
}
domain::ProcessingSupport beat_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,bpm=0,one=0,bytes=0;BeatTrackerConfig c;domain::ProcessingSupport s;
    const bool tempo=input_geometry(n,1,Rank::Vector,bpm,one) || input_geometry(n,1,Rank::Scalar,bpm,one);
    if(!input_geometry(n,0,Rank::Vector,a,b) || !tempo || (bpm!=1 && bpm!=a) ||
       !tracking_config(n,c) || !PreparedBeatTracker::storage_bytes(a,c,bytes))return {};
    bytes-=sizeof(PreparedBeatTracker);
    if(!prepared_storage::add_array(bytes,a,sizeof(std::uint8_t)) || !output_geometry(n,0,Rank::Vector,a,1,DType::UInt8) || !tracking_outputs(n,a,1))return {};
    descriptor_resources(n,p,sizeof(BeatPath),bytes,s);return s;
}
domain::ProcessingSupport envelope_pulse_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0;EnvelopeLocalPulseConfig c;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !envelope_pulse_config(n,c) ||
       !optional_vector(n,1,c.pulse.window.length/2+1) ||
       !PreparedEnvelopeLocalPulse::storage_bytes(a,c,array(n,"coefficients"),bytes) || !prepared_storage::add_array(bytes,a,sizeof(float)) ||
       !output_geometry(n,0,Rank::Vector,a) || !output_geometry(n,1,Rank::Vector,c.pulse.window.length/2+1))return {};
    descriptor_resources(n,p,sizeof(EnvelopePulse),bytes,s);return s;
}
domain::ProcessingSupport envelope_beat_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,bytes=0;EnvelopeBeatTrackingConfig c;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Vector,a,b) || !read_tracking_settings(n,c) || !PreparedEnvelopeBeatTracking::storage_bytes(a,c,bytes))return {};
    std::size_t lags=0;
    if(!tempo_window_frames(c.tempo.autocorrelation_seconds,c.tempo.sample_rate,c.tempo.hop_samples,lags) || !optional_vector(n,1,a,true) || !optional_vector(n,2,lags))return {};
    const auto type=!c.sparse?DType::UInt8:c.units==EventCoordinateUnit::Seconds?DType::Float64:DType::UInt64;
    if(!output_geometry(n,0,Rank::Vector,a,1,type) || !output_geometry(n,1,Rank::Vector,c.tempo.estimator.aggregate==TempoAggregation::None?a:1) || !tracking_outputs(n,a,2))return {};
    descriptor_resources(n,p,sizeof(EnvelopeBeatPath),bytes,s);return s;
}
domain::ProcessingSupport pulse_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t a=0,b=0,x=0,y=0,maximum=0,bytes=0,frames=0;LocalPulseConfig c;domain::ProcessingSupport s;
    if(!input_geometry(n,0,Rank::Matrix,a,b) || !input_geometry(n,1,Rank::Matrix,x,y) || a!=x || b!=y || !pulse_config(n,c) ||
       !optional_vector(n,2,c.window.length/2+1) ||
       b!=c.window.length/2+1 || !integer(n,"samples",512,maximum) || !PreparedTemporalWindow::frame_count(c.window,maximum,frames) || frames!=a ||
       !PreparedLocalPulse::storage_bytes(maximum,c,array(n,"coefficients"),bytes) || !prepared_storage::add_array(bytes,maximum,sizeof(float)) ||
       !output_geometry(n,0,Rank::Vector,maximum))return {};
    descriptor_resources(n,p,sizeof(Pulse),bytes,s);return s;
}
}
PreparedSequence prepare_beat_pulse(const domain::ConfiguredProcessingNode& node) {
    const auto n=definition(node);
    if(n.kind=="DynamicBeatPath"){auto op=std::make_unique<BeatPath>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="EnvelopeBeatPath"){auto op=std::make_unique<EnvelopeBeatPath>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="EnvelopeLocalPulse"){auto op=std::make_unique<EnvelopePulse>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="LocalPulse"){auto op=std::make_unique<Pulse>();return op->prepare(n)?std::move(op):nullptr;}
    return {};
}
domain::ProcessingSupport assess_beat_pulse(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    if(n.kind=="DynamicBeatPath")return beat_support(n,p);
    if(n.kind=="EnvelopeBeatPath")return envelope_beat_support(n,p);
    if(n.kind=="EnvelopeLocalPulse")return envelope_pulse_support(n,p);
    if(n.kind=="LocalPulse")return pulse_support(n,p);
    return {};
}
}
