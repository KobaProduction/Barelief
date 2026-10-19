#include "numeric_engine_ingress.h"
#include <QMetaObject>
#include <algorithm>
#include <cstring>
#include <limits>
#include <cmath>

namespace pipeline::numeric_engine_detail {
SourceIngress::SourceIngress(ExecutionBoundary& boundary,DeliveryHub& hub,AudioSourceFactory source)
    :execution(boundary),delivery(hub),owner(boundary.owner),mutex(boundary.mutex),state(boundary.state),executor(boundary.executor),factory(std::move(source)) {
    if(!factory){native_capture_backend=true;factory=default_audio_source_factory();}
    clock_timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&clock_timer,&QTimer::timeout,&owner,[this]{advance_clocks();});clock_timer.start(1);
}
Status SourceIngress::process(domain::NodeId node,const DataFrameView& frame,domain::PortId port) {
    return executor.process_numeric_ports(node,port,frame,{state.graph_revision,state.epoch},delivery.callbacks());
}
void SourceIngress::clear() {working_capture_bytes=0;inputs.clear();clocks.clear();external_inputs.clear();}
Status SourceIngress::emit_input(CaptureInput& input) {
        core::perf::Scope input_trace(core::perf::Stage::NumericInputProcess);
        NumericView numeric{&input.contract,input.bytes,input.planes,{input.block,1}};
        if(input.sequence==UINT64_MAX){input.gap=true;input.filled=0;return Status::overflow;}
        const auto stamp=input_stamp(input);
        const auto result=process(input.node,{stamp,numeric},input.port);
        // A descendant can reject its own computation after this PCM packet
        // was accepted and delivered to other branches. Its error is not a
        // capture gap: propagating it back here repeatedly resets resamplers
        // and rolling windows. The executor detects an actually skipped source
        // interval from its per-port acceptance stamp on the next packet.
        input.filled=0;input.gap=false;
        return result;
    }
FrameStamp SourceIngress::input_stamp(CaptureInput& input) const {
    FrameStamp stamp;stamp.sequence=stamp.source_sequence=++input.sequence;stamp.generation=input.generation;
    stamp.epoch=state.epoch;stamp.producer_epoch=input.producer_epoch;stamp.graph_revision=state.graph_revision;
    stamp.timestamp_ticks=input.ticks;stamp.sample_begin=input.ticks;stamp.sample_count=input.block;
    stamp.has_sample_interval=true;stamp.discontinuity=input.gap;
    return stamp;
}
void SourceIngress::capture(domain::NodeId node,std::uint64_t fence,const AudioFrame& frame) {
        const auto lock_start=core::perf::enabled.load(std::memory_order_relaxed)?core::perf::now():0;
        std::lock_guard lock(mutex);
        if(lock_start)core::perf::duration(core::perf::Stage::NumericCaptureLock,core::perf::now()-lock_start);
        core::perf::Scope capture_trace(core::perf::Stage::NumericCaptureWork);
        const auto token=source_tokens.find(node);
        if(token==source_tokens.end() || fence!=token->second || !executor.running())return;
        const auto resumed_epoch=resumed_capture_epoch(node,frame);
        for(auto& input:inputs)if(input.node==node)process_capture_input(input,node,fence,frame,resumed_epoch);
    }
std::uint64_t SourceIngress::resumed_capture_epoch(domain::NodeId node,const AudioFrame& frame) {
    for(const auto& input:inputs)if(input.node==node && input.sequence &&
        (input.synthetic || input.generation!=frame.generation))return state.epoch+(++capture_fence);
    return 0;
}
bool SourceIngress::capture_contract_matches(const CaptureInput& input,const AudioFrame& frame) const {
    return frame.sample_rate==input.contract.clock.sample_rate &&
        (input.select_native_lanes?(frame.channel_count>=input.source_lanes && frame.channel_count<=64):
                                   frame.channel_count==input.source_lanes) &&
        frame.frame_count && frame.frame_count<=192000/4 &&
        frame.samples.size()==frame.frame_count*frame.channel_count && frame.timestamp_ns>=0 &&
        frame.sample_position<=std::uint64_t(std::numeric_limits<std::int64_t>::max())-frame.frame_count;
}
void SourceIngress::reject_capture(CaptureInput& input,domain::NodeId node,std::uint64_t fence,const AudioFrame& frame) {
    if(!input.admission_failed) {
        input.admission_failed=true;
        const auto error=QStringLiteral("Audio capture contract mismatch: expected %1 Hz, %2 outputs; received %3 Hz, %4 lanes, %5 frames")
            .arg(input.contract.clock.sample_rate).arg(input.source_lanes).arg(frame.sample_rate)
            .arg(frame.channel_count).arg(frame.frame_count);
        QMetaObject::invokeMethod(&owner,[this,node,fence,error]{source_status(node,fence,false,error);},Qt::QueuedConnection);
    }
    input.filled=0;input.gap=true;
}
void SourceIngress::process_capture_input(CaptureInput& input,domain::NodeId node,std::uint64_t fence,
                                          const AudioFrame& frame,std::uint64_t resumed_epoch) {
    if(!capture_contract_matches(input,frame)){reject_capture(input,node,fence,frame);return;}
    if(input.admission_failed){input.admission_failed=false;
        QMetaObject::invokeMethod(&owner,[this,node,fence]{source_status(node,fence,true,{});},Qt::QueuedConnection);}
    const bool interval_gap=input.last_frame_ns && !input.synthetic &&
        frame.generation==input.generation && frame.sample_position!=input.next_sample;
    const bool capture_gap=frame.discontinuity || frame.generation!=input.generation || input.synthetic || interval_gap;
    if(capture_gap){input.filled=0;input.gap=true;}
    if(resumed_epoch)input.producer_epoch=resumed_epoch;
    input.generation=frame.generation;input.synthetic=false;input.last_frame_ns=steady_ns();
    input.clock_origin_ns=input.last_frame_ns;
    input.next_sample=frame.sample_position+frame.frame_count;input.clock_base_sample=input.next_sample;
    const auto* samples=frame.samples.data()+input.lane*frame.frame_count;
    for(std::size_t sample=0;sample<frame.frame_count;++sample)if(!std::isfinite(samples[sample])){
        input.filled=0;input.gap=true;input.dropped_samples+=frame.frame_count;input.packets.clear();return;}
    enqueue_capture_packets(input,frame,samples,capture_gap);
}
void SourceIngress::enqueue_capture_packets(CaptureInput& input,const AudioFrame& frame,const float* samples,bool capture_gap) {
    for(std::size_t offset=0;offset<frame.frame_count;) {
        const auto count=std::min(input.block,frame.frame_count-offset);
        if(input.packet_sequence==UINT64_MAX){input.gap=true;input.filled=0;break;}
        FrameStamp packet;packet.sequence=++input.packet_sequence;packet.source_sequence=frame.sequence;
        packet.producer_epoch=input.producer_epoch;packet.generation=input.generation;
        packet.timestamp_ticks=packet.sample_begin=std::int64_t(frame.sample_position+offset);
        packet.sample_count=count;packet.has_sample_interval=true;packet.discontinuity=offset==0&&capture_gap;
        const auto pushed=input.packets.push({samples+offset,count},packet);
        if(pushed!=Status::ok){input.gap=true;input.filled=0;input.dropped_samples+=count;break;}
        offset+=count;
        drain_capture_packets(input);
    }
}
void SourceIngress::drain_capture_packets(CaptureInput& input) {
    while(input.packets.stats().queued_samples){
        std::size_t written=0;FrameStamp accepted;
        const auto popped=input.packets.pop(std::span(input.packet_scratch).first(input.block-input.filled),written,accepted);
        if(popped!=Status::ok){input.gap=true;input.filled=0;break;}
        if(accepted.discontinuity){input.filled=0;input.gap=true;}
        if(!input.filled)input.ticks=accepted.sample_begin;
        std::memcpy(input.bytes.data()+input.filled*sizeof(float),input.packet_scratch.data(),written*sizeof(float));
        input.filled+=written;if(input.filled==input.block)emit_input(input);
    }
}
void SourceIngress::note_external(domain::NodeId node,const DataFrameView& frame,Status status) {
        const auto found=external_inputs.find(node);if(found==external_inputs.end() || (status!=Status::ok && status!=Status::unavailable))return;
        auto& input=found->second;
        if(frame.stamp.available)note_available_external(node,input,frame);
        else if(!input.missing) {
            input.missing=true;input.origin_ns=steady_ns();input.ordinal=0;
            input.stamp.discontinuity=true;
        }
    }
bool SourceIngress::adjust_external_interval(ExternalInput& input,const FrameStamp& stamp) {
    if(!input.block || !stamp.has_sample_interval)return true;
    const auto delta=std::int64_t(stamp.sample_count)-std::int64_t(input.block);
    const auto add=[](std::int64_t& value,std::int64_t change) {
        if((change>0 && value>INT64_MAX-change) || (change<0 && value<INT64_MIN-change))return false;
        value+=change;return true;
    };
    const bool valid=add(input.stamp.sample_begin,delta) && add(input.stamp.timestamp_ticks,delta);
    input.stamp.sample_count=input.block;
    return valid;
}
void SourceIngress::note_available_external(domain::NodeId node,ExternalInput& input,const DataFrameView& frame) {
    input.failed=false;state.statuses.remove(QStringLiteral("input:%1").arg(node));
    input.stamp=frame.stamp;input.stamp.stream_id={};
    if(!adjust_external_interval(input,frame.stamp)) {
        input.failed=true;state.statuses[QStringLiteral("input:%1").arg(node)]={State::Failed,QStringLiteral("Source clock exhausted"),state.revision};
    }
    input.missing=false;input.initial=false;
}
void SourceIngress::advance_clocks() {
        core::perf::Scope clock_tick(core::perf::Stage::NumericClockTick);
        std::lock_guard lock(mutex);if(!executor.running())return;
        const auto now=steady_ns();
        advance_sample_clocks(now);
        advance_missing_inputs(now);
        advance_failed_sources(now);
    }
void SourceIngress::advance_sample_clocks(std::uint64_t now) {
    for(auto& [id,clock]:clocks)advance_sample_clock(id,clock,now);
}
void SourceIngress::advance_sample_clock(domain::NodeId id,ClockInput& clock,std::uint64_t now) {
    if(clock.failed)return;
    const auto elapsed=now-clock.origin_ns;
    const auto target=elapsed/1000000000*clock.rate+elapsed%1000000000*clock.rate/1000000000;
    std::size_t iterations=0;
    while(clock.next_sample<=target && clock.block<=target-clock.next_sample && iterations++<32) {
        if(clock.next_sample>std::uint64_t(INT64_MAX)-clock.block || clock.sequence==UINT64_MAX) {
            clock.failed=true;state.statuses[QStringLiteral("clock:%1").arg(id)]={State::Failed,QStringLiteral("Synchronization timeline exhausted"),state.revision};break;
        }
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=++clock.sequence;
        stamp.producer_epoch=clock.producer_epoch;stamp.epoch=state.epoch;stamp.graph_revision=state.graph_revision;
        stamp.timestamp_ticks=stamp.sample_begin=std::int64_t(clock.next_sample);stamp.sample_count=clock.block;
        stamp.has_sample_interval=true;stamp.discontinuity=clock.next_sample==0;
        const auto result=executor.process_clock(id,stamp,{state.graph_revision,state.epoch},delivery.callbacks());
        if(result!=Status::ok) {
            clock.failed=true;state.statuses[QStringLiteral("clock:%1").arg(id)]={State::Failed,failure(result),state.revision};break;
        }
        state.statuses.remove(QStringLiteral("clock:%1").arg(id));clock.next_sample+=clock.block;
    }
    clock.lag_samples=target>clock.next_sample?target-clock.next_sample:0;
    clock.max_lag_samples=std::max(clock.max_lag_samples,clock.lag_samples);
    if(iterations>32)++clock.catchup_limits;
}
void SourceIngress::advance_missing_inputs(std::uint64_t now) {
        for(auto& [node,input]:external_inputs)if(input.missing && !input.failed)
            advance_missing_input(node,input,now);
    }
void SourceIngress::advance_missing_input(domain::NodeId node,ExternalInput& input,std::uint64_t now) {
            const long double period=static_cast<long double>(input.cadence.numerator)*1000000000/input.cadence.denominator;
            if(period<=0)return;
            const auto target=std::uint64_t(static_cast<long double>(now-input.origin_ns)/period);
            std::size_t iterations=0;
            while(input.ordinal<target && iterations++<32) {
                if(!emit_missing_frame(node,input))break;
            }
    }
static bool advance_stamp_value(std::int64_t& value,long double delta) {
    if(!std::isfinite(delta) || delta<0 || delta>=9223372036854775808.0L)return false;
    const auto increment=std::int64_t(delta);
    if(value>INT64_MAX-increment)return false;value+=increment;return true;
}
bool SourceIngress::emit_missing_frame(domain::NodeId node,ExternalInput& input) {
    auto stamp=input.stamp;const auto ordinal=++input.ordinal;
    if(ordinal>UINT64_MAX-stamp.sequence) {
        input.failed=true;state.statuses[QStringLiteral("input:%1").arg(node)]={State::Failed,QStringLiteral("Source clock exhausted"),state.revision};return false;
    }
    stamp.sequence+=ordinal;stamp.source_sequence=input.stamp.source_sequence;
    stamp.epoch=state.epoch;stamp.graph_revision=state.graph_revision;
    stamp.discontinuity=ordinal==1;stamp.available=true;
    const auto seconds=static_cast<long double>(ordinal)*input.cadence.numerator/input.cadence.denominator;
    const auto contract=input.contract.view();const auto& clock=std::get<NumericContract>(contract).clock;
    if(!advance_stamp_value(stamp.timestamp_ticks,seconds*clock.tick_seconds.denominator/clock.tick_seconds.numerator) ||
        (stamp.has_sample_interval && (!advance_stamp_value(stamp.sample_begin,seconds*input.rate) ||
        stamp.sample_count>std::uint64_t(INT64_MAX) || stamp.sample_begin>INT64_MAX-std::int64_t(stamp.sample_count)))) {
        input.failed=true;state.statuses[QStringLiteral("input:%1").arg(node)]={State::Failed,QStringLiteral("Source clock exhausted"),state.revision};return false;
    }
    const auto result=executor.advance_unavailable_source(node,stamp,{state.graph_revision,state.epoch},delivery.callbacks());
    if(result==Status::ok)++input.zero_frames;
    return true;
}
void SourceIngress::advance_failed_sources(std::uint64_t now) {
        for(auto& input:inputs) {
            const auto source=source_states.find(input.node);
            const bool failed=source!=source_states.end() && source->second.state==State::Failed;
            if(failed)advance_failed_source(input,now);
        }
    }
void SourceIngress::advance_failed_source(CaptureInput& input,std::uint64_t now) {
            const auto rate=input.contract.clock.sample_rate;
            if(!input.clock_origin_ns)input.clock_origin_ns=now;
            const auto elapsed=now-input.clock_origin_ns;
            const auto due=elapsed/1000000000*rate+(elapsed%1000000000)*rate/1000000000;
            // A failing source advances its accepted local fallback grid, never claims
            // synchronization with the absent hardware clock.
            if(due>UINT64_MAX-input.clock_base_sample) {
                state.statuses[QStringLiteral("input:%1").arg(input.node)]={State::Failed,QStringLiteral("Source clock exhausted"),state.revision};return;
            }
            const auto target=input.clock_base_sample+due;
            std::size_t iterations=0;
            while(input.next_sample<=target && input.block<=target-input.next_sample && iterations++<32) {
                if(input.next_sample>std::uint64_t(INT64_MAX)-input.block) {
                    state.statuses[QStringLiteral("input:%1").arg(input.node)]={State::Failed,QStringLiteral("Source clock exhausted"),state.revision};break;
                }
                input.ticks=std::int64_t(input.next_sample);
                std::fill(input.bytes.begin(),input.bytes.end(),std::byte{});input.gap|=!input.synthetic;input.synthetic=true;
                emit_input(input);input.next_sample+=input.block;
            }
            if(iterations>32){input.gap=true;input.next_sample=target;}
    }
void SourceIngress::stop_sources(){
        {std::lock_guard lock(mutex);source_tokens.clear();source_states.clear();}
        for(auto& [node,source]:sources)source.source->stop();sources.clear();
    }
void SourceIngress::update_capture_status() {
        ComponentStatus aggregate{State::Stopped,{},state.revision};
        if(capture_requested && !inputs.empty()) {
            aggregate.state=State::Running;
            for(const auto& input:inputs) {
                const auto found=source_states.find(input.node);
                if(found!=source_states.end() && found->second.state==State::Failed) {
                    aggregate.state=State::Failed;
                    if(!aggregate.error.isEmpty())aggregate.error+='\n';
                    aggregate.error+=QStringLiteral("%1: %2").arg(input.node).arg(found->second.error);
                } else if(aggregate.state!=State::Failed &&
                    (found==source_states.end() || found->second.state!=State::Running))aggregate.state=State::Starting;
            }
        }
        state.statuses[QStringLiteral("capture")]=std::move(aggregate);
    }
void SourceIngress::source_status(domain::NodeId node,std::uint64_t fence,bool running,const QString& error) {
        {std::lock_guard lock(mutex);const auto token=source_tokens.find(node);if(token==source_tokens.end() || token->second!=fence)return;
            source_states[node]={running?State::Running:State::Failed,error,state.revision};
            update_capture_status();
            if(!running)mark_source_inputs_failed(node);
        }
        if(snapshot_changed)snapshot_changed();
    }
void SourceIngress::mark_source_inputs_failed(domain::NodeId node) {
    for(auto& input:inputs)if(input.node==node) {
        input.filled=0;input.gap=true;
        if(!input.clock_origin_ns)input.clock_origin_ns=steady_ns();
    }
}
void SourceIngress::reconcile_sources() {
        const auto needed=needed_sources();
        reconcile_existing_sources(needed);
        start_needed_sources(needed);
        reproject_failed_sources();
        {std::lock_guard lock(mutex);update_capture_status();}
    }
SourceIngress::NeededSources SourceIngress::needed_sources() {
        NeededSources needed;
        {std::lock_guard lock(mutex);
            if(capture_requested)for(const auto& input:inputs)needed.emplace(input.node,std::pair{input.source,input.configuration});
            std::erase_if(source_tokens,[&](const auto& value){return !needed.contains(value.first);});
            std::erase_if(source_states,[&](const auto& value){return !needed.contains(value.first);});}
        return needed;
    }
bool SourceIngress::reconfigure_source(decltype(sources)::iterator it,const NeededSources& needed) {
        const auto next=needed.find(it->first);
        if(next==needed.end() || next->second.first!=it->second.kind ||
           !it->second.source->reconfigure_live(next->second.second))return false;
        {std::lock_guard lock(mutex);
            for(auto& input:inputs)if(input.node==it->first &&
                (!input.producer_epoch || input.source!=AudioSourceKind::TestSignal)) {
                input.producer_epoch=state.epoch+(++capture_fence);
                input.filled=0;input.gap=true;
            }
        }
        it->second.configuration=next->second.second;
        return true;
    }
void SourceIngress::reconcile_existing_sources(const NeededSources& needed) {
        for(auto it=sources.begin();it!=sources.end();) {
            const auto next=needed.find(it->first);
            if(next!=needed.end() && next->second.first==it->second.kind && next->second.second==it->second.configuration){++it;continue;}
            if(reconfigure_source(it,needed)){++it;continue;}
            {std::lock_guard lock(mutex);source_tokens.erase(it->first);source_states.erase(it->first);}
            it->second.source->stop();it=sources.erase(it);
        }
    }
std::uint64_t SourceIngress::start_source_fence(domain::NodeId node,bool available) {
        std::lock_guard lock(mutex);const auto fence=++capture_fence;source_tokens[node]=fence;
        if(available)source_states[node]={State::Starting,{},state.revision};
        for(auto& input:inputs)if(input.node==node){input.producer_epoch=state.epoch+fence;input.filled=0;}
        return fence;
    }
void SourceIngress::start_needed_sources(const NeededSources& needed) {
        for(const auto& [node,settings]:needed) {
            if(sources.contains(node))continue;
            start_source(node,settings.first,settings.second);
        }
    }
void SourceIngress::start_source(domain::NodeId node,AudioSourceKind kind,const QJsonObject& settings) {
            auto source=factory(kind);
            if(!source){const auto fence=start_source_fence(node,false);
                source_status(node,fence,false,QStringLiteral("Capture source unavailable"));return;}
            source->configure(settings);
            const auto fence=start_source_fence(node,true);
            source->set_audio_handler([this,node,fence](const AudioFrame& frame){capture(node,fence,frame);});
            source->set_status_handler([this,node,fence](bool running,QString error){
                QMetaObject::invokeMethod(&owner,[this,node,fence,running,error=std::move(error)]{
                    source_status(node,fence,running,error);},Qt::QueuedConnection);});
            source->set_lifecycle_handler([this,node,fence](bool ready,std::uint64_t,QString error){
                QMetaObject::invokeMethod(&owner,[this,node,fence,ready,error=std::move(error)]{
                    source_status(node,fence,ready,error);},Qt::QueuedConnection);});
            source->start();sources.emplace(node,ActiveSource{kind,settings,std::move(source)});
    }
void SourceIngress::reproject_failed_sources() {
        // A compatible Apply or newly admitted preview demand can expose a
        // retained failed source without reopening it or receiving a callback.
        struct FailedSource {domain::NodeId node;std::uint64_t fence;QString error;};
        std::vector<FailedSource> failed;
        {std::lock_guard lock(mutex);for(const auto& [node,status]:source_states)
            if(capture_requested&&status.state==State::Failed)if(const auto token=source_tokens.find(node);token!=source_tokens.end())
                failed.push_back({node,token->second,status.error});}
        for(const auto& source:failed)source_status(source.node,source.fence,false,source.error);
    }
}
