#pragma once
#include <core_app/numeric_madmom_reference_graph.h>
#include <dataflow_runtime/runtime.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace madmom_whole_fixture {
using namespace signal_contracts;
using namespace dataflow_runtime;
using Check=void(*)(bool,const char*);

class Collector {
    Check check_;
    std::vector<float> values_;
    std::size_t count_=0;
    void validate(const NumericPortFrameView& event,const NumericView& value) const {
        check_(value.contract->dtype==DType::Float32 && value.contract->shape.rank==Rank::Scalar &&
            value.actual==std::array<std::size_t,2>{1,1},"whole ODF producer is one Float32 scalar");
        const auto& clock=value.contract->clock;
        check_(clock.domain=="local" && clock.sample_rate==48000 && clock.tick_seconds==Ratio{1,48000},"producer PCM clock");
        const auto& stamp=event.frame.stamp;
        const auto expected=std::int64_t(count_*240);
        if(stamp.timestamp_ticks!=expected)std::fprintf(stderr,"madmom ODF frame=%zu timestamp=%lld expected=%lld sequence=%llu source_sequence=%llu\n",
            count_,static_cast<long long>(stamp.timestamp_ticks),static_cast<long long>(expected),
            static_cast<unsigned long long>(stamp.sequence),static_cast<unsigned long long>(stamp.source_sequence));
        check_(stamp.timestamp_ticks==expected,"producer timestamp is complete-frame left edge");
        check_(!stamp.has_sample_interval && !stamp.sample_begin && !stamp.sample_count,"scalar ODF has point stamp");
        check_(stamp.discontinuity==(count_==0),"first finite ODF discontinuity only");
    }
public:
    Collector(std::size_t count,Check check):check_(check),values_(count) {}
    void observe(const NumericPortFrameView& event) {
        if(event.direction!=domain::PortDirection::Output || event.identity.node!=3 || event.identity.port)return;
        if(event.status==Status::unavailable && !event.frame.stamp.available)return;
        check_(event.status==Status::ok && event.frame.stamp.available,"whole ODF available producer");
        check_(count_<values_.size(),"finite ODF never exceeds declared F");
        const auto* value=std::get_if<NumericView>(&event.frame.payload);check_(value,"numeric ODF producer");
        validate(event,*value);
        float sample=0;check_(load_element(*value,{},sample)==Status::ok && std::isfinite(sample) && sample>=0,"finite ODF sample");
        values_[count_++]=sample;
    }
    bool complete() const noexcept {return count_==values_.size();}
    std::size_t count() const noexcept {return count_;}
    std::span<const float> values() const noexcept {return std::span(values_).first(count_);}
};

template<class Observer> class Session {
    Check check_;
    Observer& seen_;
    Collector collector_;
    std::size_t expected_,processed_=0;
    std::uint64_t sequence_=0;
    bool finalized_=false;
    std::shared_ptr<const PreparedGraph> plan_;
    std::unique_ptr<RuntimeContext> state_;
    std::array<float,8192> pcm_{};
    static void observe(void* user,const NumericPortFrameView& event) {
        auto& self=*static_cast<Session*>(user);
        self.collector_.observe(event);Observer::output(&self.seen_,event);
    }
    NumericRuntimeCallbacks callbacks() {return {observe,nullptr,nullptr,this};}
    NumericContract contract(domain::NodeId id) const {
        const auto value=plan_->port_contract(id,domain::PortDirection::Output,0);
        check_(value.has_value() && std::holds_alternative<NumericContract>(*value),"finite ingress contract");
        return std::get<NumericContract>(*value);
    }
    void validate_complete_contract() const {
        const auto value=contract(31);
        check_(value.dtype==DType::Float32 && value.shape.rank==Rank::Vector && value.channel_order.size()==1,"complete ODF vector");
        check_(value.shape.extents[0]==ExtentBounds{collector_.values().size(),collector_.values().size()},"exact actual F contract");
        check_(value.clock.domain=="madmom-whole-odf" && value.clock.sample_rate==200 && value.clock.tick_seconds==Ratio{1,200},"whole ODF frame clock");
        check_(value.coordinates[0]==AxisCoordinates{CoordinateMode::Regular,NumericUnit::Seconds,{0,1},{1,200}},"whole ODF left-edge coordinates");
    }
    Status packet(std::span<const float> input) {
        const auto value=contract(1);const PlaneLayout layout{0,{sizeof(float),0}};
        std::copy(input.begin(),input.end(),pcm_.begin());
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence_;stamp.epoch=stamp.producer_epoch=1;
        stamp.discontinuity=processed_==0;stamp.has_sample_interval=true;stamp.sample_count=input.size();
        stamp.timestamp_ticks=stamp.sample_begin=std::int64_t(processed_);stamp.stream_id="whole-native-pcm";
        NumericView view{&value,std::as_bytes(std::span(pcm_)),{&layout,1},{input.size(),1}};
        const auto result=plan_->process_numeric_ports(1,{stamp,view},*state_,{},callbacks());
        processed_+=input.size();return result;
    }
    Status submit_complete() {
        const auto value=contract(31);const PlaneLayout layout{0,{sizeof(float),0}};
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=1;stamp.epoch=stamp.producer_epoch=stamp.generation=1;
        stamp.discontinuity=true;stamp.stream_id="madmom-whole-odf";
        NumericView view{&value,std::as_bytes(collector_.values()),{&layout,1},{collector_.count(),1}};
        return plan_->process_numeric_ports(31,{stamp,view},*state_,{},callbacks());
    }
public:
    Session(const domain::NumericProcessingGraph& graph,const NumericRuntimeProfile& profile,
            std::size_t samples,std::size_t frames,Observer& seen,Check check)
        :check_(check),seen_(seen),collector_(frames,check),expected_(samples) {
        check_(PreparedGraph::prepare_numeric(graph,profile,plan_,state_)==Status::ok,"prepare whole finite graph");
        const auto source=contract(1);
        check_(source.shape.extents[0].maximum<=pcm_.size() && source.shape.extents[0].minimum<=1,"owned PCM ingress envelope");
    }
    Status feed(std::span<const float> input,std::size_t packet_size) {
        if(finalized_ || !packet_size || packet_size>pcm_.size() || input.size()>expected_-processed_)return Status::invalid_argument;
        for(std::size_t offset=0;offset<input.size();){
            const auto count=std::min(packet_size,input.size()-offset);
            const auto result=packet(input.subspan(offset,count));if(result!=Status::ok)return result;
            offset+=count;
        }
        return Status::ok;
    }
    Status finish() {
        if(finalized_ || processed_!=expected_ || !collector_.complete())return Status::invalid_argument;
        validate_complete_contract();finalized_=true;return submit_complete();
    }
    std::size_t frames() const noexcept {return collector_.count();}
    std::size_t samples() const noexcept {return processed_;}
};
}
