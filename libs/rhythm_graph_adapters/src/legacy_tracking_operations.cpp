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
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n,const TrackingPlan& p){configure_plan(p);values_.resize(p.rows);const auto g=array_parameter(n,"gains");return kernel_.prepare(g.empty()?std::span<const float>(default_tempo_band_gains):g,p.frame_time);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+values_.capacity()*sizeof(float);}
private:
    void reset_kernel() noexcept override {kernel_.reset();}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {float cumulative=0;return kernel_.process(in[0].contiguous_float32,values_,cumulative) && batch_vector(out[0],values_) && batch_scalar(out[1],cumulative);}
    PreparedBandTempoFeatures kernel_;std::vector<float> values_;
};
class PhaseTicks final:public TrackingKernel<PhaseTicks> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.rows,p.maximum_matches,p.maximum_ticks,p.ticks);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    void reset_kernel() noexcept override {kernel_.reset(true);}
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(!kernel_.process(in[0].contiguous_float32,in[1].contiguous_float32))return false;
        return batch_vector(out[0],kernel_.ticks()) && batch_vector(out[1],kernel_.matching_periods()) && scalar(out[2],kernel_.next_phase()) && batch_integer(out[3],kernel_.consumed_frames()) && batch_status(out[4],std::uint8_t(kernel_.status()));
    }
    PreparedPeriodPhaseTicks kernel_;
};
class CombResponse final:public TrackingKernel<CombResponse> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan& p){configure_plan(p);values_.resize(p.response_length);row_.resize(p.matrix?p.columns:0);return true;}
    std::size_t workspace_bytes() const noexcept override {return (values_.capacity()+row_.capacity())*sizeof(float);}
private:
    bool response(std::span<const float> values,std::span<const NumericOperationInput> in) noexcept {
        const auto& p=tracking_;
        return p.kind==TrackingKind::HarmonicComb?
            harmonic_comb_response(values,in[1].contiguous_float32,p.first,p.stop,p.harmonics,values_):
            periodic_phase_response(values,p.period,p.harmonics,values_);
    }
    bool matrix(std::span<const NumericOperationInput> in,NumericOperationOutput& out) noexcept {
        const auto source=batch_rows(in[0]);out.value.actual={in[0].value.actual[0],values_.size()};
        for(std::size_t row=0;row<out.value.actual[0];++row){
            if(!source.read(source.context,row,row_) || !response(row_,in))return false;
            for(std::size_t column=0;column<values_.size();++column)
                if(store_element(out.value,{0,row,column},values_[column])!=Status::ok)return false;
        }
        return true;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        if(tracking_.matrix)return matrix(in,out[0]);
        return response(in[0].contiguous_float32,in) && batch_vector(out[0],values_);
    }
    std::vector<float> values_,row_;
};
class Silence final:public TrackingKernel<Silence> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan& p){configure_plan(p);frame_.resize(p.rows);return kernel_.prepare(p.rows,p.silence);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes()+frame_.capacity()*sizeof(float);}
    Status process(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out,NumericOperationControl c) noexcept override {
        invalidate_outputs(out);
        if(unsigned(c.event)>unsigned(NumericOperationEvent::EndOfStream))return Status::invalid_argument;
        auto status=validate_targets(in,out);if(status!=Status::ok)return status;
        if(!c.enabled)return zeros(out,operation_frame_stamp(input_at(in,0),false));
        return process_enabled(in,out,c);
    }
private:
    bool finish_stream(NumericOperationEvent event) noexcept {
        if(event!=NumericOperationEvent::EndOfStream || finished_)return true;
        if(!kernel_.finish())return false;
        finished_=true;return true;
    }
    Status process_enabled(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out,NumericOperationControl control) noexcept {
        bool frame=false;auto status=validate_inputs(in,control.event,frame);if(status!=Status::ok)return status;
        if(control.reset)reset_kernel();
        if((frame && !run(in,out)) || !finish_stream(control.event))return Status::invalid_argument;
        const auto stamp=have_frame_?stamp_:operation_frame_stamp(input_at(in,tracking_control_port),false);
        status=publish(out,stamp,frame);if(status!=Status::ok)invalidate_outputs(out);return status;
    }
    void reset_kernel() noexcept override {kernel_.reset();finished_=false;have_frame_=false;stamp_={};}
    Status validate_targets(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) const noexcept {
        if(!output_contracts_match(out) || !proof_buffers_disjoint(in,out) || !disjoint_operation_buffers(in,out))return Status::incompatible_contract;
        for(const auto& output:out){const auto status=validate_mutable(output.value,limits_);if(status!=Status::ok)return status;}
        return Status::ok;
    }
    Status validate_input(const NumericOperationInput& value,const dataflow_runtime::NumericOperationNodeView& node,
        const NumericBufferContract& expected,NumericOperationEvent event,bool& frame) const noexcept {
        if(value.port==tracking_control_port)return validate_operation_control(value,expected,limits_);
        if(event==NumericOperationEvent::EndOfStream && value.status==Status::unavailable)return Status::ok;
        const auto status=validate_frame_vector(value,node,tracking_.rows,limits_,false);if(status!=Status::ok)return status;
        frame=true;return Status::ok;
    }
    Status validate_input_at(std::span<const NumericOperationInput> inputs,std::size_t index,const dataflow_runtime::NumericOperationNodeView& node,
        NumericOperationEvent event,bool& frame) const noexcept {
        const auto& value=inputs[index];NumericBufferContract expected;
        if(!numeric_port(node,true,value.port,expected))return Status::incompatible_contract;
        if(!operation_input_unique(inputs,index))return Status::invalid_argument;
        return validate_input(value,node,expected,event,frame);
    }
    Status validate_inputs(std::span<const NumericOperationInput> in,NumericOperationEvent event,bool& frame) const noexcept {
        const auto node=definition_view(descriptor_->node);frame=false;
        for(std::size_t i=0;i<in.size();++i){
            const auto status=validate_input_at(in,i,node,event,frame);if(status!=Status::ok)return status;
        }
        return frame || event==NumericOperationEvent::EndOfStream?Status::ok:Status::unavailable;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        (void)out;const auto* input=input_at(in,0);if(finished_ || !input || input->contiguous_float32.size()!=frame_.size())return false;
        std::copy(input->contiguous_float32.begin(),input->contiguous_float32.end(),frame_.begin());
        if(!kernel_.process(frame_,tracking_.leading,tracking_.acquired))return false;
        stamp_=operation_frame_stamp(input,true);have_frame_=true;return true;
    }
    bool publish_bounds(std::span<NumericOperationOutput> out) const noexcept {
        const auto bounds=kernel_.bounds();const std::array<std::uint64_t,3> values{bounds.start,bounds.stop,bounds.frames};
        for(std::size_t i=0;i<values.size();++i)if(!batch_integer(*output_at(out,domain::PortId(i+1)),values[i]))return false;
        return true;
    }
    Status bind_publication(std::span<NumericOperationOutput> out,const dataflow_runtime::NumericOperationOrigin& stamp,bool frame) const noexcept {
        for(auto& output:out){
            if(!output.port && !frame)continue;
            const auto status=validate_view(output.value.view(),limits_);if(status!=Status::ok)return status;
            output.origin=stamp;output.status=Status::ok;
        }
        return Status::ok;
    }
    Status publish(std::span<NumericOperationOutput> out,dataflow_runtime::NumericOperationOrigin stamp,bool frame) const noexcept {
        if(frame && !batch_vector(*output_at(out,0),frame_))return Status::invalid_argument;
        if(!publish_bounds(out))return Status::invalid_argument;
        if(!frame)stamp=numeric_operation_origin(stamp,NumericOperationPublication::Point);
        return bind_publication(out,stamp,frame);
    }
    Status zeros(std::span<NumericOperationOutput> out,dataflow_runtime::NumericOperationOrigin stamp) const noexcept {

        for(auto& output:out){
            const auto& shape=tracking_.shape[output.port];output.value.actual={shape.rows,shape.columns};
            const auto status=fill_silence(output.value,limits_);if(status!=Status::ok){invalidate_outputs(out);return status;}
            output.origin=stamp;output.status=Status::ok;
        }
        return Status::ok;
    }
    PreparedFrameSilencePolicy kernel_;std::vector<float> frame_;dataflow_runtime::NumericOperationOrigin stamp_;bool finished_=false,have_frame_=false;
};
class LagPool final:public TrackingKernel<LagPool> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.maximum_rows,p.maximum_matches,p.maximum_matches);}
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
    bool prepare(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.maximum_ticks,p.refinement);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        return kernel_.process(in[0].contiguous_float32,tracking_.total_frames,tracking_.start_silence,tracking_.stop_silence) && batch_vector(out[0],kernel_.ticks()) && batch_vector(out[1],kernel_.intervals()) && batch_status(out[2],std::uint8_t(kernel_.status()));
    }
    PreparedTickSequenceRefinement kernel_;
};
class LagVote final:public TrackingKernel<LagVote> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView&,const TrackingPlan& p){configure_plan(p);return kernel_.prepare(p.rows,p.vote);}
    std::size_t workspace_bytes() const noexcept override {return kernel_.workspace_bytes();}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {return kernel_.process(in[0].contiguous_float32) && batch_scalar(out[0],kernel_.bpm()) && batch_vector(out[1],kernel_.estimates());}
    PreparedLagTempoVote kernel_;
};
template<class T> std::unique_ptr<AdapterOperation> make(const dataflow_runtime::NumericOperationNodeView& n,const TrackingPlan& p){auto v=std::make_unique<T>();return v->prepare(n,p)?std::move(v):nullptr;}
}
std::unique_ptr<AdapterOperation> prepare_legacy_tracking(const dataflow_runtime::NumericOperationNodeView& n,const TrackingPlan& p){
    switch(p.kind){case TrackingKind::BandFeatures:return make<BandFeatures>(n,p);case TrackingKind::PhaseTicks:return make<PhaseTicks>(n,p);
    case TrackingKind::HarmonicComb:case TrackingKind::PhaseResponse:return make<CombResponse>(n,p);case TrackingKind::Silence:return make<Silence>(n,p);case TrackingKind::LagPool:return make<LagPool>(n,p);case TrackingKind::TickRefinement:return make<TickRefinement>(n,p);case TrackingKind::LagVote:return make<LagVote>(n,p);default:return {};}
}
std::size_t legacy_tracking_object_bytes(TrackingKind k) noexcept {
    switch(k){case TrackingKind::BandFeatures:return sizeof(BandFeatures);case TrackingKind::PhaseTicks:return sizeof(PhaseTicks);
    case TrackingKind::HarmonicComb:case TrackingKind::PhaseResponse:return sizeof(CombResponse);case TrackingKind::Silence:return sizeof(Silence);case TrackingKind::LagPool:return sizeof(LagPool);case TrackingKind::TickRefinement:return sizeof(TickRefinement);case TrackingKind::LagVote:return sizeof(LagVote);default:return 0;}
}
} // namespace rhythm_graph_adapters::detail
