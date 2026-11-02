#include "tempo_evidence_execution.h"
#include "node_state.h"
#include "frame_access.h"
#include "preparation_support.h"
#include "signal_time.h"
#include <domain/music/contracts.h>
#include <dsp_kernels/tempo_evidence.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dataflow_runtime::detail {
namespace r=dsp_kernels::rhythm;
namespace {
bool same_session(const FrameStamp& a,const FrameStamp& b) noexcept {
    return a.producer_epoch==b.producer_epoch && a.generation==b.generation && a.epoch==b.epoch && a.graph_revision==b.graph_revision;
}
bool same_interval(const FrameStamp& a,const FrameStamp& b) noexcept {
    return same_session(a,b) && a.timestamp_ticks==b.timestamp_ticks && a.sequence==b.sequence &&
        a.source_sequence==b.source_sequence && a.has_sample_interval==b.has_sample_interval &&
        (!a.has_sample_interval || (a.sample_begin==b.sample_begin && a.sample_count==b.sample_count));
}
bool older(const FrameStamp& a,const FrameStamp& b) noexcept {
    return same_session(a,b) && (a.timestamp_ticks<b.timestamp_ticks || (a.timestamp_ticks==b.timestamp_ticks && a.sequence<b.sequence));
}
double coordinate(std::span<const std::byte> bytes,std::size_t index) noexcept {
    std::uint64_t bits=0;
    for(std::size_t byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[index*8+byte]))<<(8*byte);
    double result=0;std::memcpy(&result,&bits,8);return result;
}
bool fourier_coordinates(const NumericView& source,std::size_t size,unsigned rate) noexcept {
    if(source.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)return true;
    const auto bytes=source.coordinates.axes[0];
    if(bytes.size()!=source.actual[0]*8)return false;
    for(std::size_t bin=0;bin<source.actual[0];++bin){
        const auto hz=coordinate(bytes,bin),expected=double(bin)*rate/double(size);
        if(!std::isfinite(hz) || std::abs(hz-expected)>1e-9*std::max(1.,expected))return false;
    }
    return true;
}
Status read_curve(const NumericView& source,std::span<float> target,const FrameLimits& limits) noexcept {
    if(source.ring.capacity || source.actual[0]>target.size() || source.actual[1]!=1)return Status::incompatible_contract;
    const auto valid=validate_values(source,ValueDomain::FiniteOnly,limits);if(valid!=Status::ok)return valid;
    for(std::size_t bin=0;bin<source.actual[0];++bin){
        const auto status=load_element(source,{0,bin,0},target[bin]);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
std::array<float,domain::tempo_candidate_columns> values(const r::TempoEvidenceCandidate& c) noexcept {
    return {c.bpm,c.score,c.fourier_short,c.fourier_long,c.acf_short,c.acf_long,float(c.origin_curves)};
}
Status candidate_matrix(const r::TempoEvidenceOutput& result,MutableNumericView& destination) noexcept {
    for(std::size_t row=0;row<result.candidates.size();++row){
        const auto record=values(row<result.count?result.candidates[row]:r::TempoEvidenceCandidate{});
        for(std::size_t column=0;column<record.size();++column){
            const auto status=store_element(destination,{0,row,column},record[column]);if(status!=Status::ok)return status;
        }
    }
    return Status::ok;
}
Status scalar_result(const r::TempoEvidenceOutput& result,MutableNumericView& destination,PortId port) noexcept {
    switch(port){
    case 1:return store_element(destination,{},std::uint32_t(result.count));
    case 2:return store_element(destination,{},result.maturity);
    case 3:return store_element(destination,{},std::uint8_t(result.harmonic_ambiguity));
    default:return Status::invalid_argument;
    }
}
Status publish(const PlanNode& node,State& state,const FrameStamp& stamp,const r::TempoEvidenceOutput& result) noexcept {
    for(PortId port=0;port<domain::tempo_evidence_outputs;++port){
        auto destination=mutable_output(node,state,port);
        const auto status=port?scalar_result(result,destination,port):candidate_matrix(result,destination);
        if(status!=Status::ok)return status;
        state.numeric_frames[port]={stamp,destination.view()};
    }
    return Status::ok;
}
}
struct PreparedTempoEvidenceOperation::Impl {
    r::PreparedTempoEvidence kernel;
    r::TempoEvidenceConfig config;
    std::array<std::vector<float>,4> curves;
    std::array<std::size_t,4> lengths{};
    std::array<std::uint64_t,2> filled{};
    std::array<bool,domain::tempo_evidence_inputs> pending{};
    FrameStamp interval{};
    bool observed=false,published=false;

    bool begin(const FrameStamp& stamp) noexcept {
        if(observed && same_interval(stamp,interval))return !published;
        if(observed && older(stamp,interval))return false;
        pending={};interval=stamp;interval.stream_id={};observed=true;published=false;return true;
    }
    void combine(const FrameStamp& stamp) noexcept {
        interval.discontinuity=interval.discontinuity || stamp.discontinuity;
        interval.frequency_dependent_latency=interval.frequency_dependent_latency || stamp.frequency_dependent_latency;
        if(latency_greater(stamp.latency_seconds,interval.latency_seconds))interval.latency_seconds=stamp.latency_seconds;
    }
    Status retain(std::size_t port,const NumericView& source,const FrameLimits& limits) noexcept {
        if(port>=4)return load_element(source,{},filled[port-4]);
        if(port<2 && !fourier_coordinates(source,port?config.long_fft:config.short_fft,config.frame_rate_fps))return Status::incompatible_contract;
        if(port>=2 && source.actual[0]<2)return Status::unavailable;
        const auto status=read_curve(source,curves[port],limits);
        if(status==Status::ok)lengths[port]=source.actual[0];
        return status;
    }
    void missing(const PlanNode& node) noexcept {
        for(const auto& input:node.endpoint_plan->inputs){
            if(input.parent!=absent)continue;
            const auto port=input.port;
            if(port<4){std::fill(curves[port].begin(),curves[port].end(),0.f);lengths[port]=curves[port].size();}
            else filled[port-4]=0;
            pending[port]=true;
        }
    }
    Status retain_input(const PlanNode& node,std::size_t lane,const DataFrameView& current,const FrameLimits& limits) noexcept {
        if(!begin(current.stamp))return Status::would_block;
        const auto port=node.endpoint_plan->inputs[lane].port;
        const auto status=node.node.enabled?retain(port,std::get<NumericView>(current.payload),limits):Status::ok;
        if(status!=Status::ok)return status;
        pending[port]=true;combine(current.stamp);missing(node);return Status::ok;
    }
    Status complete(const PlanNode& node,State& state) noexcept {
        if(!std::all_of(pending.begin(),pending.end(),[](bool ready){return ready;}))return Status::would_block;
        if(!node.node.enabled){published=true;return publish(node,state,interval,r::TempoEvidenceOutput{});}
        if(filled[0]>config.short_fft || filled[1]>config.long_fft)return Status::invalid_argument;
        const r::PeriodicityCurve short_curve{std::span(curves[0]).first(lengths[0]),std::span(curves[2]).first(lengths[2]),std::size_t(filled[0])};
        const r::PeriodicityCurve long_curve{std::span(curves[1]).first(lengths[1]),std::span(curves[3]).first(lengths[3]),std::size_t(filled[1])};
        if(!kernel.process(short_curve,long_curve))return Status::invalid_argument;
        published=true;
        return publish(node,state,interval,kernel.output());
    }
};
PreparedTempoEvidenceOperation::PreparedTempoEvidenceOperation():value_(std::make_unique<Impl>()){}
PreparedTempoEvidenceOperation::~PreparedTempoEvidenceOperation()=default;
PreparedTempoEvidenceOperation::PreparedTempoEvidenceOperation(const PreparedTempoEvidenceOperation& other):value_(std::make_unique<Impl>(*other.value_)){}
PreparedTempoEvidenceOperation& PreparedTempoEvidenceOperation::operator=(const PreparedTempoEvidenceOperation& other){if(this!=&other)value_=std::make_unique<Impl>(*other.value_);return *this;}
PreparedTempoEvidenceOperation::PreparedTempoEvidenceOperation(PreparedTempoEvidenceOperation&&) noexcept=default;
PreparedTempoEvidenceOperation& PreparedTempoEvidenceOperation::operator=(PreparedTempoEvidenceOperation&&) noexcept=default;
void PreparedTempoEvidenceOperation::reset() noexcept {value_->pending={};value_->observed=value_->published=false;}
bool PreparedTempoEvidenceOperation::prepare(const ResolvedProcessingNode& node) {
    domain::TempoEvidenceParameters p;if(!domain::tempo_evidence_parameters(node,p))return false;
    auto next=std::make_unique<Impl>();
    next->config={unsigned(p.frame_rate_fps),p.short_fft,p.long_fft,p.min_bpm,p.max_bpm,p.min_observation_s,p.mature_observation_s};
    if(!next->kernel.prepare(next->config))return false;
    for(const auto& input:node.inputs)if(input.id<4){
        const auto data=input.contract.view();next->curves[input.id].resize(std::size_t(std::get<NumericContract>(data).shape.extents[0].maximum));
    }
    value_=std::move(next);return true;
}
Status PreparedTempoEvidenceOperation::accept(const PlanNode& node,State& state,std::size_t lane,const DataFrameView& current,const FrameLimits& limits) noexcept {
    if(lane>=node.endpoint_plan->inputs.size())return Status::invalid_argument;
    if(!current.stamp.available){reset();return Status::unavailable;}
    const auto status=value_->retain_input(node,lane,current,limits);
    if(status!=Status::ok){if(status!=Status::would_block)reset();return status;}
    const auto completed=value_->complete(node,state);
    if(completed!=Status::ok && completed!=Status::would_block)reset();
    return completed;
}
bool PreparedTempoEvidenceOperation::resources(const ResolvedProcessingNode& node,ProcessingResources& result) noexcept {
    std::size_t bytes=sizeof(Impl);
    for(const auto& input:node.inputs)if(input.id<4){
        const auto data=input.contract.view();std::size_t payload=0;
        if(!numeric_mul(std::size_t(std::get<NumericContract>(data).shape.extents[0].maximum),sizeof(float),payload) || !numeric_add(bytes,payload))return false;
    }
    return numeric_add(result.payload_bytes,bytes);
}
}
