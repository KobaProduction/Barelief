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
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=++input.sequence;stamp.generation=input.generation;
        stamp.epoch=state.epoch;stamp.producer_epoch=input.producer_epoch;stamp.graph_revision=state.graph_revision;
        stamp.timestamp_ticks=input.ticks;stamp.sample_begin=input.ticks;stamp.sample_count=input.block;
        stamp.has_sample_interval=true;stamp.discontinuity=input.gap;
        const auto result=process(input.node,{stamp,numeric},input.port);
        input.filled=0;if(result==Status::ok)input.gap=false;
        return result;
    }
void SourceIngress::capture(domain::NodeId node,std::uint64_t fence,const AudioFrame& frame) {
        const auto lock_start=core::perf::enabled.load(std::memory_order_relaxed)?core::perf::now():0;
        std::lock_guard lock(mutex);
        if(lock_start)core::perf::duration(core::perf::Stage::NumericCaptureLock,core::perf::now()-lock_start);
        core::perf::Scope capture_trace(core::perf::Stage::NumericCaptureWork);
        const auto token=source_tokens.find(node);
        if(token==source_tokens.end() || fence!=token->second || !executor.running())return;
        std::uint64_t resumed_epoch=0;
        for(const auto& input:inputs)if(input.node==node && input.sequence &&
            (input.synthetic || input.generation!=frame.generation)) {resumed_epoch=state.epoch+(++capture_fence);break;}
        for(auto& input:inputs)if(input.node==node) {
            if(frame.sample_rate!=input.contract.clock.sample_rate || frame.channel_count!=input.source_lanes ||
                frame.frame_count>192000/4 || frame.samples.size()!=frame.frame_count*frame.channel_count ||
                frame.timestamp_ns<0 || frame.sample_position>std::uint64_t(std::numeric_limits<std::int64_t>::max())-frame.frame_count) {
                if(!input.admission_failed) {
                    input.admission_failed=true;
                    const auto error=QStringLiteral("Audio capture contract mismatch: expected %1 Hz, %2 outputs; received %3 Hz, %4 lanes, %5 frames")
                        .arg(input.contract.clock.sample_rate).arg(input.source_lanes).arg(frame.sample_rate).arg(frame.channel_count).arg(frame.frame_count);
                    QMetaObject::invokeMethod(&owner,[this,node,fence,error]{source_status(node,fence,false,error);},Qt::QueuedConnection);
                }
                input.filled=0;input.gap=true;continue;
            }
            if(input.admission_failed) {
                input.admission_failed=false;
                QMetaObject::invokeMethod(&owner,[this,node,fence]{source_status(node,fence,true,{});},Qt::QueuedConnection);
            }
            // The sample interval is authoritative even when a provider forgets
            // the optional discontinuity hint. Never join partial blocks across
            // a missing/overlapping interval and relabel them as contiguous PCM.
            const bool interval_gap=input.last_frame_ns && !input.synthetic &&
                frame.generation==input.generation && frame.sample_position!=input.next_sample;
            if(frame.discontinuity || frame.generation!=input.generation || input.synthetic || interval_gap){input.filled=0;input.gap=true;}
            if(resumed_epoch)input.producer_epoch=resumed_epoch;
            input.generation=frame.generation;input.synthetic=false;input.last_frame_ns=steady_ns();
            input.clock_origin_ns=input.last_frame_ns;
            input.next_sample=frame.sample_position+frame.frame_count;input.clock_base_sample=input.next_sample;
            const auto* samples=frame.samples.data()+input.lane*frame.frame_count;
            for(std::size_t sample=0;sample<frame.frame_count;++sample) {
                if(!input.filled)input.ticks=std::int64_t(frame.sample_position+sample);
                const float value=samples[sample];
                std::memcpy(input.bytes.data()+input.filled*sizeof(float),&value,sizeof(value));
                if(++input.filled==input.block)emit_input(input);
            }
        }
    }
void SourceIngress::note_external(domain::NodeId node,const DataFrameView& frame,Status status) {
        const auto found=external_inputs.find(node);if(found==external_inputs.end() || (status!=Status::ok && status!=Status::unavailable))return;
        auto& input=found->second;
        if(frame.stamp.available) {
            input.failed=false;state.statuses.remove(QStringLiteral("input:%1").arg(node));
            input.stamp=frame.stamp;input.stamp.stream_id={}; // Never retain a borrowed ID.
            if(input.block && frame.stamp.has_sample_interval) {
                const auto delta=std::int64_t(frame.stamp.sample_count)-std::int64_t(input.block);
                const auto add=[](std::int64_t& value,std::int64_t delta) {
                    if((delta>0 && value>INT64_MAX-delta) || (delta<0 && value<INT64_MIN-delta))return false;
                    value+=delta;return true;
                };
                if(!add(input.stamp.sample_begin,delta) || !add(input.stamp.timestamp_ticks,delta)) {
                    input.failed=true;state.statuses[QStringLiteral("input:%1").arg(node)]={State::Failed,QStringLiteral("Source clock exhausted"),state.revision};
                }
                input.stamp.sample_count=input.block;
            }
            input.missing=false;input.initial=false;
        } else if(!input.missing) {
            input.missing=true;input.origin_ns=steady_ns();input.ordinal=0;
            input.stamp.discontinuity=true;
        }
    }
void SourceIngress::advance_clocks() {
        core::perf::Scope clock_tick(core::perf::Stage::NumericClockTick);
        std::lock_guard lock(mutex);if(!executor.running())return;
        const auto now=steady_ns();
        for(auto& [id,clock]:clocks) {
            if(clock.failed)continue;
            const auto elapsed=now-clock.origin_ns;
            const auto target=elapsed/1000000000*clock.rate+elapsed%1000000000*clock.rate/1000000000;
            std::size_t iterations=0;
            while(clock.next_sample<=target && clock.block<=target-clock.next_sample && iterations++<32) {
                if(clock.next_sample>std::uint64_t(INT64_MAX)-clock.block || clock.sequence==UINT64_MAX) {
                    clock.failed=true;state.statuses[QStringLiteral("clock:%1").arg(id)]={State::Failed,QStringLiteral("Sample clock exhausted"),state.revision};break;
                }
                FrameStamp stamp;stamp.sequence=stamp.source_sequence=++clock.sequence;
                stamp.producer_epoch=clock.producer_epoch;stamp.epoch=state.epoch;stamp.graph_revision=state.graph_revision;
                stamp.timestamp_ticks=stamp.sample_begin=std::int64_t(clock.next_sample);stamp.sample_count=clock.block;
                stamp.has_sample_interval=true;stamp.discontinuity=clock.next_sample==0;
                const auto result=executor.process_clock(id,stamp,{state.graph_revision,state.epoch},
                    delivery.callbacks());
                if(result!=Status::ok) {
                    clock.failed=true;state.statuses[QStringLiteral("clock:%1").arg(id)]={State::Failed,failure(result),state.revision};break;
                }
                state.statuses.remove(QStringLiteral("clock:%1").arg(id));clock.next_sample+=clock.block;
            }
            clock.lag_samples=target>clock.next_sample?target-clock.next_sample:0;
            clock.max_lag_samples=std::max(clock.max_lag_samples,clock.lag_samples);
            if(iterations>32)++clock.catchup_limits;
        }
        for(auto& [node,input]:external_inputs)if(input.missing && !input.failed) {
            const long double period=static_cast<long double>(input.cadence.numerator)*1000000000/input.cadence.denominator;
            if(period<=0)continue;
            const auto target=std::uint64_t(static_cast<long double>(now-input.origin_ns)/period);
            std::size_t iterations=0;
            while(input.ordinal<target && iterations++<32) {
                auto stamp=input.stamp;const auto ordinal=++input.ordinal;
                if(ordinal>UINT64_MAX-stamp.sequence) {
                    input.failed=true;state.statuses[QStringLiteral("input:%1").arg(node)]={State::Failed,QStringLiteral("Source clock exhausted"),state.revision};break;
                }
                stamp.sequence+=ordinal;stamp.source_sequence=input.stamp.source_sequence;
                stamp.epoch=state.epoch;stamp.graph_revision=state.graph_revision;
                stamp.discontinuity=ordinal==1;stamp.available=true;
                const auto seconds=static_cast<long double>(ordinal)*input.cadence.numerator/input.cadence.denominator;
                const auto contract=input.contract.view();const auto& clock=std::get<NumericContract>(contract).clock;
                const auto advance=[](std::int64_t& value,long double delta) {
                    // Windows long double can have double precision. Reject the
                    // rounded 2^63 boundary before converting to signed integer.
                    if(!std::isfinite(delta) || delta<0 || delta>=9223372036854775808.0L)return false;
                    const auto increment=std::int64_t(delta);
                    if(value>INT64_MAX-increment)return false;value+=increment;return true;
                };
                if(!advance(stamp.timestamp_ticks,seconds*clock.tick_seconds.denominator/clock.tick_seconds.numerator) ||
                    (stamp.has_sample_interval && (!advance(stamp.sample_begin,seconds*input.rate) ||
                    stamp.sample_count>std::uint64_t(INT64_MAX) || stamp.sample_begin>INT64_MAX-std::int64_t(stamp.sample_count)))) {
                    input.failed=true;state.statuses[QStringLiteral("input:%1").arg(node)]={State::Failed,QStringLiteral("Source clock exhausted"),state.revision};break;
                }
                const auto result=executor.advance_unavailable_source(node,stamp,{state.graph_revision,state.epoch},
                    delivery.callbacks());
                if(result==Status::ok)++input.zero_frames;
            }
        }
        for(auto& input:inputs) {
            const auto source=source_states.find(input.node);
            const bool failed=source!=source_states.end() && source->second.state==State::Failed;
            if(!failed)continue;
            const auto rate=input.contract.clock.sample_rate;
            if(!input.clock_origin_ns)input.clock_origin_ns=now;
            const auto elapsed=now-input.clock_origin_ns;
            const auto due=elapsed/1000000000*rate+(elapsed%1000000000)*rate/1000000000;
            // A failing source advances its accepted local fallback grid, never claims
            // synchronization with the absent hardware clock.
            if(due>UINT64_MAX-input.clock_base_sample) {
                state.statuses[QStringLiteral("input:%1").arg(input.node)]={State::Failed,QStringLiteral("Source clock exhausted"),state.revision};continue;
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
            if(!running)for(auto& input:inputs)if(input.node==node) {
                // A known contract advances zeros; it is not an unavailable graph.
                input.filled=0;input.gap=true;
                if(!input.clock_origin_ns)input.clock_origin_ns=steady_ns();
            }
        }
        if(snapshot_changed)snapshot_changed();
    }
void SourceIngress::reconcile_sources() {
        std::map<domain::NodeId,std::pair<AudioSourceKind,QJsonObject>> needed;
        {std::lock_guard lock(mutex);
            if(capture_requested)for(const auto& input:inputs)needed.emplace(input.node,std::pair{input.source,input.configuration});
            std::erase_if(source_tokens,[&](const auto& value){return !needed.contains(value.first);});
            std::erase_if(source_states,[&](const auto& value){return !needed.contains(value.first);});}
        for(auto it=sources.begin();it!=sources.end();) {
            const auto next=needed.find(it->first);
            if(next!=needed.end() && next->second.first==it->second.kind && next->second.second==it->second.configuration){++it;continue;}
            if(next!=needed.end() && next->second.first==it->second.kind &&
                it->second.source->reconfigure_live(next->second.second)) {
                // A replaced capture input restarts its sequence during prepare,
                // while a live source keeps the callback token. Give that new
                // capture stream an epoch newer than the retained executor input.
                // TestSignal edits retain the existing input counters instead.
                {std::lock_guard lock(mutex);
                    for(auto& input:inputs)if(input.node==it->first &&
                        (!input.producer_epoch || input.source!=AudioSourceKind::TestSignal)) {
                        input.producer_epoch=state.epoch+(++capture_fence);
                        input.filled=0;input.gap=true;
                    }
                }
                it->second.configuration=next->second.second;++it;continue;
            }
            {std::lock_guard lock(mutex);source_tokens.erase(it->first);source_states.erase(it->first);}
            it->second.source->stop();it=sources.erase(it);
        }
        for(const auto& [node,settings]:needed) {
            if(sources.contains(node))continue;
            const auto kind=settings.first;
            auto source=factory(kind);if(!source){
                std::uint64_t fence;
                {std::lock_guard lock(mutex);fence=++capture_fence;source_tokens[node]=fence;
                    for(auto& input:inputs)if(input.node==node){input.producer_epoch=state.epoch+fence;input.filled=0;}}
                source_status(node,fence,false,QStringLiteral("Capture source unavailable"));continue;
            }
            source->configure(settings.second);
            std::uint64_t fence;
            {std::lock_guard lock(mutex);fence=++capture_fence;source_tokens[node]=fence;
                source_states[node]={State::Starting,{},state.revision};
                for(auto& input:inputs)if(input.node==node){input.producer_epoch=state.epoch+fence;input.filled=0;}}
            source->set_audio_handler([this,node,fence](const AudioFrame& frame){capture(node,fence,frame);});
            source->set_status_handler([this,node,fence](bool running,QString error){
                QMetaObject::invokeMethod(&owner,[this,node,fence,running,error=std::move(error)]{
                    source_status(node,fence,running,error);},Qt::QueuedConnection);});
            source->set_lifecycle_handler([this,node,fence](bool ready,std::uint64_t,QString error){
                QMetaObject::invokeMethod(&owner,[this,node,fence,ready,error=std::move(error)]{
                    source_status(node,fence,ready,error);},Qt::QueuedConnection);});
            source->start();sources.emplace(node,ActiveSource{kind,settings.second,std::move(source)});
        }
        reproject_failed_sources();
        {std::lock_guard lock(mutex);update_capture_status();}
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
