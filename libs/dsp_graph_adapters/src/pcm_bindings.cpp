#include "sequence_settings.h"
#include <dsp_kernels/pcm_tempo.h>
#include <algorithm>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
bool pcm_input(const domain::ResolvedProcessingNode& n,std::size_t& samples) noexcept {
    std::size_t b=0;return input_geometry(n,0,Rank::Vector,samples,b);
}
bool pcm_tempo(const domain::ResolvedProcessingNode& n,PcmTempoConfig& c) noexcept {
    return read_pcm_settings(n,c.onset) && read_envelope_settings(n,c.tempo);
}
bool pcm_ratios(const domain::ResolvedProcessingNode& n,PcmTempoRatiosConfig& c) noexcept {
    if(!read_pcm_settings(n,c.onset) || !read_envelope_ratio_settings(n,c.ratios))return false;
    c.ratios.tempogram.frame_rate=c.onset.spectrum.sample_rate/float(c.onset.onset.strength.hop_length);
    c.ratios.ratios.tempo.frame_rate=c.ratios.tempogram.frame_rate;return true;
}
bool pcm_beats(const domain::ResolvedProcessingNode& n,PcmBeatTrackingConfig& c) noexcept {
    return read_pcm_settings(n,c.onset,true) && read_tracking_settings(n,c.beats);
}
bool pcm_pulse(const domain::ResolvedProcessingNode& n,PcmLocalPulseConfig& c) noexcept {
    return read_pcm_settings(n,c.onset,true) && read_pulse_settings(n,c.pulse);
}
bool pcm_tempogram(const domain::ResolvedProcessingNode& n,PcmTempogramConfig& c) noexcept {
    std::size_t method=0;
    if(!read_pcm_settings(n,c.onset) || !integer(n,"method",0,method) || method>2 ||
       !integer(n,"window_length",384,c.tempogram.autocorrelation.window.length) || !integer(n,"hop",1,c.tempogram.autocorrelation.window.hop) ||
       !flag(n,"center",true,c.tempogram.autocorrelation.window.center))return false;
    c.tempogram.method=TempogramMethod(method);c.tempogram.frame_rate=c.onset.spectrum.sample_rate/float(c.onset.onset.strength.hop_length);return true;
}
std::optional<std::span<const float>> ratios(const domain::ResolvedProcessingNode& n) noexcept {
    return parameter(n,"factors")?std::optional(array(n,"factors")):std::nullopt;
}
bool event_result(const BeatTrackingSequenceResult& r,NumericOperationOutput& out) noexcept {
    if(out.value.contract->dtype==DType::UInt8)return write_mask(out,r.dense);
    if(out.value.contract->dtype==DType::Float64)return write_seconds(out,r.seconds);
    return r.samples.empty()?write_indices(out,r.frames):write_samples(out,r.samples);
}
class PcmOnsets final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<PcmOnsets>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t samples=0;PcmOnsetStrengthConfig c;
        return pcm_input(n,samples) && read_pcm_settings(n,c) && kernel_.prepare(samples,c,array(n,"spectral_window"));
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        return kernel_.process(input_at(in,0)->contiguous_float32) && write_values(*output_at(out,0),kernel_.values());
    }
    PreparedPcmOnsetEnvelope kernel_;
};
class PcmTempo final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<PcmTempo>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t samples=0;PcmTempoConfig c;
        if(!pcm_input(n,samples) || !pcm_tempo(n,c) || !kernel_.prepare(samples,c,array(n,"spectral_window")))return false;
        output_.resize(kernel_.output_frames(samples));return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+output_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto pcm=input_at(in,0)->contiguous_float32;const auto* prior=available_input_at(in,1);auto result=std::span(output_).first(kernel_.output_frames(pcm.size()));
        return kernel_.process(pcm,result,prior?prior->contiguous_float32:std::span<const float>{}) &&
            write_values(*output_at(out,0),result) && write_values(*output_at(out,1),kernel_.onset_envelope());
    }
    PreparedPcmTempo kernel_;std::vector<float> output_;
};
class PcmRatios final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<PcmRatios>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t samples=0;PcmTempoRatiosConfig c;
        return pcm_input(n,samples) && pcm_ratios(n,c) && kernel_.prepare(samples,c,array(n,"spectral_window"),array(n,"coefficients"),array(n,"coordinates"),ratios(n));
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto pcm=input_at(in,0)->contiguous_float32;const auto* bpm=available_input_at(in,1);const auto* prior=available_input_at(in,2);const auto* frequency=available_input_at(in,3);auto& target=*output_at(out,0);
        return set_shape(target,kernel_.output_frames(pcm.size()),kernel_.factors()) && kernel_.process(pcm,rows(target),
            bpm?std::optional(bpm->contiguous_float32):std::nullopt,prior?prior->contiguous_float32:std::span<const float>{},frequency?rows(*frequency):FloatMatrixSource{}) &&
            write_values(*output_at(out,1),kernel_.onset_envelope());
    }
    PreparedPcmTempoRatios kernel_;
};
class PcmBeats final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<PcmBeats>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t samples=0;PcmBeatTrackingConfig c;
        return pcm_input(n,samples) && pcm_beats(n,c) && kernel_.prepare(samples,c,array(n,"spectral_window"));
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto* bpm=available_input_at(in,1);const auto* prior=available_input_at(in,2);BeatTrackingSequenceResult result;
        return kernel_.process(input_at(in,0)->contiguous_float32,result,bpm?std::optional(bpm->contiguous_float32):std::nullopt,
            prior?prior->contiguous_float32:std::span<const float>{}) && event_result(result,*output_at(out,0)) &&
            write_values(*output_at(out,1),result.bpm) && write_values(*output_at(out,2),kernel_.onset_envelope());
    }
    PreparedPcmBeatTracking kernel_;
};
class PcmPulse final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<PcmPulse>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t samples=0;PcmLocalPulseConfig c;
        if(!pcm_input(n,samples) || !pcm_pulse(n,c) || !kernel_.prepare(samples,c,array(n,"spectral_window"),array(n,"coefficients")))return false;
        output_.resize(kernel_.output_frames(samples));return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+output_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto pcm=input_at(in,0)->contiguous_float32;const auto* prior=available_input_at(in,1);auto result=std::span(output_).first(kernel_.output_frames(pcm.size()));
        return kernel_.process(pcm,result,prior?prior->contiguous_float32:std::span<const float>{}) && write_values(*output_at(out,0),result) &&
            write_values(*output_at(out,1),kernel_.onset_envelope()) && write_values(*output_at(out,2),kernel_.coordinates_bpm());
    }
    PreparedPcmLocalPulse kernel_;std::vector<float> output_;
};
class PcmTempogram final:public SequenceOperation {
public:
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> clone() const override {return std::make_unique<PcmTempogram>(*this);}
    bool prepare(const domain::ResolvedProcessingNode& n) {
        std::size_t samples=0;PcmTempogramConfig c;
        if(!pcm_input(n,samples) || !pcm_tempogram(n,c) || !kernel_.prepare(samples,c,array(n,"spectral_window"),array(n,"coefficients")))return false;
        settings_=c;return true;
    }
    std::size_t object_bytes() const noexcept override {return sizeof(*this);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    struct Sink {NumericOperationOutput* real;NumericOperationOutput* imaginary;bool complex;};
    static bool write(void* context,std::size_t row,std::span<const float> values) noexcept {
        const auto& sink=*static_cast<const Sink*>(context);const auto count=sink.complex?values.size()/2:values.size();
        if(row>=sink.real->value.actual[0] || count!=sink.real->value.actual[1])return false;
        for(std::size_t k=0;k<count;++k)
            if(store_element(sink.real->value,{0,row,k},values[sink.complex?2*k:k])!=Status::ok ||
               store_element(sink.imaginary->value,{0,row,k},sink.complex?values[2*k+1]:0.f)!=Status::ok)return false;
        return true;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto pcm=input_at(in,0)->contiguous_float32;std::size_t envelope=0,count=0;
        if(!PreparedPcmOnsetEnvelope::geometry(pcm.size(),settings_.onset,{},envelope) || !PreparedTempogramSequence::frame_count(settings_.tempogram,envelope,count))return false;
        const bool complex=settings_.tempogram.method==TempogramMethod::Fourier;const auto cols=complex?kernel_.columns()/2:kernel_.columns();
        auto& real=*output_at(out,0);auto& imaginary=*output_at(out,1);Sink sink{&real,&imaginary,complex};
        return set_shape(real,count,cols) && set_shape(imaginary,count,cols) && kernel_.process(pcm,{&sink,write}) &&
            write_values(*output_at(out,2),kernel_.onset_envelope()) && write_values(*output_at(out,3),kernel_.coordinates_bpm());
    }
    PreparedPcmTempogram kernel_;PcmTempogramConfig settings_;
};
domain::ProcessingSupport onsets_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t samples=0,frames=0,bytes=0;PcmOnsetStrengthConfig c;domain::ProcessingSupport s;
    if(!pcm_input(n,samples) || !read_pcm_settings(n,c) || !PreparedPcmOnsetEnvelope::geometry(samples,c,{},frames) ||
       !PreparedPcmOnsetEnvelope::storage_bytes(samples,c,array(n,"spectral_window"),{},bytes) || !output_geometry(n,0,Rank::Vector,frames))return {};
    descriptor_resources(n,p,sizeof(PcmOnsets),bytes,s);return s;
}
domain::ProcessingSupport tempo_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t samples=0,frames=0,bytes=0;PcmTempoConfig c;domain::ProcessingSupport s;
    if(!pcm_input(n,samples) || !pcm_tempo(n,c) || !PreparedPcmOnsetEnvelope::geometry(samples,c.onset,{},frames) ||
       !PreparedPcmTempo::storage_bytes(samples,c,array(n,"spectral_window"),bytes))return {};
    const auto count=c.tempo.estimator.aggregate==TempoAggregation::None?frames:1;
    std::size_t lags=0;
    if(!tempo_window_frames(c.tempo.autocorrelation_seconds,c.tempo.sample_rate,c.tempo.hop_samples,lags) || !optional_vector(n,1,lags))return {};
    if(!prepared_storage::add_array(bytes,count,sizeof(float)) || !output_geometry(n,0,Rank::Vector,count) || !output_geometry(n,1,Rank::Vector,frames))return {};
    descriptor_resources(n,p,sizeof(PcmTempo),bytes,s);return s;
}
domain::ProcessingSupport ratio_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t samples=0,frames=0,temporal=0,bytes=0;PcmTempoRatiosConfig c;domain::ProcessingSupport s;const auto factors=ratios(n);
    if(!pcm_input(n,samples) || !pcm_ratios(n,c) || !PreparedPcmOnsetEnvelope::geometry(samples,c.onset,{},frames) ||
       !PreparedTempogramSequence::frame_count(c.ratios.tempogram,frames,temporal) ||
       !PreparedPcmTempoRatios::storage_bytes(samples,c,array(n,"spectral_window"),array(n,"coefficients"),array(n,"coordinates"),factors,bytes) ||
       !output_geometry(n,0,Rank::Matrix,c.ratios.ratios.time_reduction.operation==FeatureReduction::None?temporal:1,factors?factors->size():13) ||
       !output_geometry(n,1,Rank::Vector,frames))return {};
    if(!optional_vector(n,1,temporal,true) || !optional_vector(n,2,c.ratios.tempogram.autocorrelation.window.length))return {};
    const auto frequency=contract(n,domain::PortDirection::Input,3);
    if(c.ratios.ratios.dynamic_frequencies && !frequency)return {};
    if(frequency){std::size_t x=0,y=0;if(!input_geometry(n,3,Rank::Matrix,x,y) || x!=temporal || y!=c.ratios.tempogram.autocorrelation.window.length)return {};}
    descriptor_resources(n,p,sizeof(PcmRatios),bytes,s);return s;
}
domain::ProcessingSupport beat_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t samples=0,frames=0,bytes=0;PcmBeatTrackingConfig c;domain::ProcessingSupport s;
    if(!pcm_input(n,samples) || !pcm_beats(n,c) || !PreparedPcmOnsetEnvelope::geometry(samples,c.onset,{},frames) ||
       !PreparedPcmBeatTracking::storage_bytes(samples,c,array(n,"spectral_window"),bytes))return {};
    const auto type=!c.beats.sparse?DType::UInt8:c.beats.units==EventCoordinateUnit::Seconds?DType::Float64:DType::UInt64;
    std::size_t lags=0;
    if(!tempo_window_frames(c.beats.tempo.autocorrelation_seconds,c.beats.tempo.sample_rate,c.beats.tempo.hop_samples,lags) ||
       !optional_vector(n,1,frames,true) || !optional_vector(n,2,lags))return {};
    if(!output_geometry(n,0,Rank::Vector,frames,1,type) ||
       !output_geometry(n,1,Rank::Vector,c.beats.tempo.estimator.aggregate==TempoAggregation::None?frames:1) || !output_geometry(n,2,Rank::Vector,frames))return {};
    descriptor_resources(n,p,sizeof(PcmBeats),bytes,s);return s;
}
domain::ProcessingSupport pulse_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t samples=0,frames=0,bytes=0;PcmLocalPulseConfig c;domain::ProcessingSupport s;
    if(!pcm_input(n,samples) || !pcm_pulse(n,c) || !PreparedPcmOnsetEnvelope::geometry(samples,c.onset,{},frames) ||
       !PreparedPcmLocalPulse::storage_bytes(samples,c,array(n,"spectral_window"),array(n,"coefficients"),bytes) ||
       !prepared_storage::add_array(bytes,frames,sizeof(float)) || !output_geometry(n,0,Rank::Vector,frames) ||
       !output_geometry(n,1,Rank::Vector,frames) || !output_geometry(n,2,Rank::Vector,c.pulse.pulse.window.length/2+1))return {};
    if(!optional_vector(n,1,c.pulse.pulse.window.length/2+1))return {};
    descriptor_resources(n,p,sizeof(PcmPulse),bytes,s);return s;
}
domain::ProcessingSupport tempogram_support(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    std::size_t samples=0,frames=0,temporal=0,bytes=0;PcmTempogramConfig c;domain::ProcessingSupport s;
    if(!pcm_input(n,samples) || !pcm_tempogram(n,c) || !PreparedPcmOnsetEnvelope::geometry(samples,c.onset,{},frames) ||
       !PreparedTempogramSequence::frame_count(c.tempogram,frames,temporal) ||
       !PreparedPcmTempogram::storage_bytes(samples,c,array(n,"spectral_window"),array(n,"coefficients"),bytes))return {};
    const auto columns=c.tempogram.method==TempogramMethod::Autocorrelation?c.tempogram.autocorrelation.window.length:c.tempogram.autocorrelation.window.length/2+1;
    if(!output_geometry(n,0,Rank::Matrix,temporal,columns) || !output_geometry(n,1,Rank::Matrix,temporal,columns) ||
       !output_geometry(n,2,Rank::Vector,frames) || !output_geometry(n,3,Rank::Vector,columns))return {};
    descriptor_resources(n,p,sizeof(PcmTempogram),bytes,s);return s;
}
}
PreparedSequence prepare_pcm(const domain::ConfiguredProcessingNode& node) {
    const auto n=definition(node);
    if(n.kind=="PcmOnsetEnvelope"){auto op=std::make_unique<PcmOnsets>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="PcmTempo"){auto op=std::make_unique<PcmTempo>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="PcmTempoRatios"){auto op=std::make_unique<PcmRatios>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="PcmBeatPath"){auto op=std::make_unique<PcmBeats>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="PcmLocalPulse"){auto op=std::make_unique<PcmPulse>();return op->prepare(n)?std::move(op):nullptr;}
    if(n.kind=="PcmTempogram"){auto op=std::make_unique<PcmTempogram>();return op->prepare(n)?std::move(op):nullptr;}
    return {};
}
domain::ProcessingSupport assess_pcm(const domain::ResolvedProcessingNode& n,const dataflow_runtime::NumericRuntimeProfile& p) noexcept {
    if(n.kind=="PcmOnsetEnvelope")return onsets_support(n,p);
    if(n.kind=="PcmTempo")return tempo_support(n,p);
    if(n.kind=="PcmTempoRatios")return ratio_support(n,p);
    if(n.kind=="PcmBeatPath")return beat_support(n,p);
    if(n.kind=="PcmLocalPulse")return pulse_support(n,p);
    if(n.kind=="PcmTempogram")return tempogram_support(n,p);
    return {};
}
}
