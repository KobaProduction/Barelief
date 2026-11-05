// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tracking_operations.h"
#include "frame_operation_io.h"
#include <rhythm_reference_ports/periodic_comb.h>
#include <algorithm>
namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;using namespace dataflow_runtime;using namespace signal_contracts;
namespace {
template<class T> bool scalar(NumericOperationOutput& o,T v) noexcept {o.value.actual={1,1};return store_element(o.value,{0,0,0},v)==Status::ok;}
class BandFeatures final:public TrackingKernel<BandFeatures> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n,const TrackingPlan& p){configure_plan(p);values_.resize(p.rows);const auto g=array_parameter(n,"gains");return kernel_.prepare(g.empty()?std::span<const float>(default_tempo_band_gains):g,p.frame_time);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+values_.capacity()*sizeof(float);}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {float cumulative=0;return kernel_.process(in[0].contiguous_float32,values_,cumulative) && batch_vector(out[0],values_) && batch_scalar(out[1],cumulative);}
    PreparedBandTempoFeatures kernel_;std::vector<float> values_;
};
class CombPeriod final:public TrackingKernel<CombPeriod> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n,const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.comb,array_parameter(n,"tempo_hints_seconds"));}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        float period=0,phase=-1;return kernel_.process_feature(in[0].contiguous_float32,period,phase) && batch_scalar(out[0],period) && batch_scalar(out[1],phase) && batch_vector(out[2],kernel_.correlation()) && batch_vector(out[3],kernel_.weights()) && batch_vector(out[4],kernel_.comb()) && batch_vector(out[5],kernel_.phase_response());
    }
    PreparedCombPeriodPhase kernel_;
};
class FeaturePeriod final:public TrackingKernel<FeaturePeriod> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n,const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.rows,p.legacy.evaluation_hop,p.comb,array_parameter(n,"tempo_hints_seconds"));}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {return kernel_.push(in[0].contiguous_float32) && batch_vector(out[0],kernel_.periods()) && batch_vector(out[1],kernel_.phases()) && batch_status(out[2],kernel_.ready()?1:0);}
    PreparedFeaturePeriodTracker kernel_;
};
class PhaseTicks final:public TrackingKernel<PhaseTicks> {
public:
    bool prepare(const domain::ResolvedProcessingNode&,const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.rows,p.maximum_matches,p.maximum_ticks,p.ticks);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset(true);}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto a=in[0].value.coordinates.axes[0],b=in[1].value.coordinates.axes[0];
        if(a.size()!=b.size() || !std::equal(a.begin(),a.end(),b.begin()) || !kernel_.process(in[0].contiguous_float32,in[1].contiguous_float32))return false;
        return batch_vector(out[0],kernel_.ticks()) && batch_vector(out[1],kernel_.matching_periods()) && scalar(out[2],kernel_.next_phase()) && batch_integer(out[3],kernel_.consumed_frames()) && batch_status(out[4],std::uint8_t(kernel_.status()));
    }
    PreparedPeriodPhaseTicks kernel_;
};
class CombResponse final:public TrackingKernel<CombResponse> {
public:
    bool prepare(const domain::ResolvedProcessingNode&,const TrackingPlan& p){configure_plan(p);values_.resize(p.response_length);return true;}
    std::size_t workspace_bytes() const noexcept override {return values_.capacity()*sizeof(float);}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto& p=tracking_;const bool valid=p.kind==TrackingKind::HarmonicComb?harmonic_comb_response(in[0].contiguous_float32,in[1].contiguous_float32,p.first,p.stop,p.harmonics,values_):periodic_phase_response(in[0].contiguous_float32,p.period,p.harmonics,values_);
        return valid && batch_vector(out[0],values_);
    }
    std::vector<float> values_;
};
class Silence final:public TrackingKernel<Silence> {
public:
    bool prepare(const domain::ResolvedProcessingNode&,const TrackingPlan& p){configure_plan(p);frame_.resize(p.rows);return kernel_.prepare(p.rows,p.silence);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+frame_.capacity()*sizeof(float);}
    Status process(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out,NumericOperationControl c) noexcept override {
        invalidate_outputs(out);
        if(unsigned(c.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
        auto status=validate_targets(in,out);if(status!=Status::ok)return status;
        if(!c.enabled)return zeros(out,operation_frame_stamp(input_at(in,0),false));
        bool frame=false;status=validate_inputs(in,c.event,frame);if(status!=Status::ok)return status;
        if(c.reset)reset_kernel();
        if(frame && !run(in,out))return Status::invalid_argument;
        if(c.event==NumericOperationEvent::EndOfStream && !finished_){if(!kernel_.finish())return Status::invalid_argument;finished_=true;}
        const auto stamp=have_frame_?stamp_:operation_frame_stamp(input_at(in,tracking_control_port),false);
        status=publish(out,stamp,frame);if(status!=Status::ok)invalidate_outputs(out);return status;
    }
private:
    void reset_kernel() noexcept override {kernel_.reset();finished_=false;have_frame_=false;stamp_={};}
    Status validate_targets(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) const noexcept {
        if(!output_contracts_match(out) || !proof_buffers_disjoint(in,out) || !disjoint_operation_buffers(in,out))return Status::incompatible_contract;
        for(const auto& output:out){const auto status=validate_mutable(output.value,limits_);if(status!=Status::ok)return status;}
        return Status::ok;
    }
    Status validate_inputs(std::span<const NumericOperationInput> in,NumericOperationEvent event,bool& frame) const noexcept {
        const auto node=definition_view(descriptor_->node);frame=false;
        for(std::size_t i=0;i<in.size();++i){
            const auto& value=in[i];NumericContract expected;
            if(!numeric_port(node,true,value.port,expected))return Status::incompatible_contract;
            for(std::size_t j=0;j<i;++j)if(in[j].port==value.port)return Status::invalid_argument;
            if(value.port==tracking_control_port){const auto status=validate_operation_control(value,expected,limits_);if(status!=Status::ok)return status;continue;}
            if(event==NumericOperationEvent::EndOfStream && (value.status==Status::unavailable || !value.stamp.available))continue;
            const auto status=validate_frame_vector(value,node,tracking_.rows,limits_,false);if(status!=Status::ok)return status;
            frame=true;
        }
        return frame || event==NumericOperationEvent::EndOfStream?Status::ok:Status::unavailable;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        (void)out;const auto* input=input_at(in,0);if(finished_ || !input || input->contiguous_float32.size()!=frame_.size())return false;
        std::copy(input->contiguous_float32.begin(),input->contiguous_float32.end(),frame_.begin());
        if(!kernel_.process(frame_,tracking_.leading,tracking_.acquired))return false;
        stamp_=operation_frame_stamp(input,true);have_frame_=true;return true;
    }
    Status publish(std::span<NumericOperationOutput> out,FrameStamp stamp,bool frame) const noexcept {
        const auto bounds=kernel_.bounds();const std::array<std::uint64_t,3> values{bounds.start,bounds.stop,bounds.frames};
        if(frame && !batch_vector(*output_at(out,0),frame_))return Status::invalid_argument;
        for(std::size_t i=0;i<values.size();++i)if(!batch_integer(*output_at(out,domain::PortId(i+1)),values[i]))return Status::invalid_argument;
        if(!frame){stamp.has_sample_interval=false;stamp.sample_begin=0;stamp.sample_count=0;}
        stamp.available=true;
        for(auto& output:out){
            if(!output.port && !frame)continue;
            const auto status=validate_view({stamp,output.value.view()},limits_);if(status!=Status::ok)return status;
            output.stamp=stamp;output.status=Status::ok;
        }
        return Status::ok;
    }
    Status zeros(std::span<NumericOperationOutput> out,FrameStamp stamp) const noexcept {
        stamp.available=true;
        for(auto& output:out){
            const auto& shape=tracking_.shape[output.port];output.value.actual={shape.rows,shape.columns};
            const auto status=fill_silence(output.value,limits_);if(status!=Status::ok){invalidate_outputs(out);return status;}
            output.stamp=stamp;output.status=Status::ok;
        }
        return Status::ok;
    }
    PreparedFrameSilencePolicy kernel_;std::vector<float> frame_;FrameStamp stamp_;bool finished_=false,have_frame_=false;
};
class LagPool final:public TrackingKernel<LagPool> {
public:
    bool prepare(const domain::ResolvedProcessingNode&,const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.maximum_rows,p.maximum_matches,p.maximum_matches);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(!kernel_.push(in[0].contiguous_float32) || !batch_vector(out[0],kernel_.unique_values()))return false;
        const auto source=kernel_.source();const auto n=kernel_.flattened_size();out[1].value.actual={n,1};
        for(std::size_t i=0;i<n;++i){float v=0;if(!source.read(source.context,i,v) || store_element(out[1].value,{0,i,0},v)!=Status::ok)return false;}
        return batch_integer(out[2],kernel_.rows()) && batch_integer(out[3],n);
    }
    PreparedLagObservationPool kernel_;
};
class TickRefinement final:public TrackingKernel<TickRefinement> {
public:
    bool prepare(const domain::ResolvedProcessingNode&,const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.maximum_ticks,p.refinement);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        return kernel_.process(in[0].contiguous_float32,tracking_.total_frames,tracking_.start_silence,tracking_.stop_silence) && batch_vector(out[0],kernel_.ticks()) && batch_vector(out[1],kernel_.intervals()) && batch_status(out[2],std::uint8_t(kernel_.status()));
    }
    PreparedTickSequenceRefinement kernel_;
};
class LagVote final:public TrackingKernel<LagVote> {
public:
    bool prepare(const domain::ResolvedProcessingNode&,const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.rows,p.vote);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {return kernel_.process(in[0].contiguous_float32) && batch_scalar(out[0],kernel_.bpm()) && batch_vector(out[1],kernel_.estimates());}
    PreparedLagTempoVote kernel_;
};
class Legacy final:public TrackingKernel<Legacy> {
public:
    bool prepare(const domain::ResolvedProcessingNode& n,const TrackingPlan& p){configure_plan(p);return kernel_.prepare({p.rows,p.maximum_ticks,p.maximum_matches},p.legacy,array_parameter(n,"tempo_hints_seconds"));}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset(true);}
    bool bounds(NumericOperationOutput& out) const noexcept {const auto b=kernel_.silence_bounds();out.value.actual={3,1};return store_element(out.value,{0,0,0},b.start)==Status::ok && store_element(out.value,{0,1,0},b.stop)==Status::ok && store_element(out.value,{0,2,0},b.frames)==Status::ok;}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(!kernel_.process(in[0].contiguous_float32))return false;const auto s=kernel_.frame_stages();
        return batch_vector(out[0],kernel_.ticks()) && batch_vector(out[1],kernel_.intervals()) && batch_scalar(out[2],kernel_.bpm()) && batch_vector(out[3],kernel_.bpm_estimates()) && batch_status(out[4],std::uint8_t(kernel_.status())) &&
            batch_vector(out[5],s.features) && batch_vector(out[6],s.band_power) && batch_vector(out[7],s.periods) && batch_vector(out[8],s.phases) && batch_vector(out[9],s.ticks) && batch_vector(out[10],s.matching_periods) && bounds(out[11]) && tracking_frame_outputs(out,12,s.spectrum);
    }
    PreparedLegacyRhythmRecipe kernel_;
};
template<class T> std::unique_ptr<AdapterOperation> make(const domain::ResolvedProcessingNode& n,const TrackingPlan& p){auto v=std::make_unique<T>();return v->prepare(n,p)?std::move(v):nullptr;}
}
std::unique_ptr<AdapterOperation> prepare_legacy_tracking(const domain::ResolvedProcessingNode& n,const TrackingPlan& p){
    switch(p.kind){case TrackingKind::BandFeatures:return make<BandFeatures>(n,p);case TrackingKind::CombPeriod:return make<CombPeriod>(n,p);case TrackingKind::FeaturePeriod:return make<FeaturePeriod>(n,p);case TrackingKind::PhaseTicks:return make<PhaseTicks>(n,p);
    case TrackingKind::HarmonicComb:case TrackingKind::PhaseResponse:return make<CombResponse>(n,p);case TrackingKind::Silence:return make<Silence>(n,p);case TrackingKind::LagPool:return make<LagPool>(n,p);case TrackingKind::TickRefinement:return make<TickRefinement>(n,p);case TrackingKind::LagVote:return make<LagVote>(n,p);case TrackingKind::Legacy:return make<Legacy>(n,p);default:return {};}
}
std::size_t legacy_tracking_object_bytes(TrackingKind k) noexcept {
    switch(k){case TrackingKind::BandFeatures:return sizeof(BandFeatures);case TrackingKind::CombPeriod:return sizeof(CombPeriod);case TrackingKind::FeaturePeriod:return sizeof(FeaturePeriod);case TrackingKind::PhaseTicks:return sizeof(PhaseTicks);
    case TrackingKind::HarmonicComb:case TrackingKind::PhaseResponse:return sizeof(CombResponse);case TrackingKind::Silence:return sizeof(Silence);case TrackingKind::LagPool:return sizeof(LagPool);case TrackingKind::TickRefinement:return sizeof(TickRefinement);case TrackingKind::LagVote:return sizeof(LagVote);case TrackingKind::Legacy:return sizeof(Legacy);default:return 0;}
}
} // namespace rhythm_graph_adapters::detail
