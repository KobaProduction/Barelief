#include "numeric_engine_delivery.h"
#include <QMetaObject>
#include <QPointer>
#include <algorithm>
#include <limits>

namespace pipeline::numeric_engine_detail {
dataflow_runtime::NumericRuntimeCallbacks DeliveryHub::callbacks() noexcept {
    return {[](void* p,const auto& f){static_cast<DeliveryHub*>(p)->output(f);},
            [](void* p,const auto& f){static_cast<DeliveryHub*>(p)->output(f);},
            [](void* p,const auto& f){static_cast<DeliveryHub*>(p)->history(f);},this};
}
static std::uint64_t milliseconds_now() noexcept {return steady_ns()/1000000;}
DeliveryHub::DeliveryHub(ExecutionBoundary& boundary):execution(boundary),owner(boundary.owner),mutex(boundary.mutex),state(boundary.state) {
    preview_timer.setTimerType(Qt::PreciseTimer);
    preview_timer.setSingleShot(true);preview_timer.setInterval(16);
    QObject::connect(&preview_timer,&QTimer::timeout,&owner,[this]{
        if(preview_timer_armed_ns)core::perf::duration(core::perf::Stage::NumericPreviewTimerWait,steady_ns()-preview_timer_armed_ns);
        dispatch_previews();
    });
    wled_timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&wled_timer,&QTimer::timeout,&owner,[this]{flush_wled();});
}
bool DeliveryHub::tap_selected(const EndpointIdentity& endpoint) const {
        for(const auto& [id,tap]:taps)if(tap.endpoints.empty() ||
            std::find(tap.endpoints.begin(),tap.endpoints.end(),endpoint)!=tap.endpoints.end())return true;
        return false;
    }
bool DeliveryHub::selected(const EndpointIdentity& endpoint) const {
        return tap_selected(endpoint)||std::find(subscriptions.begin(),subscriptions.end(),endpoint)!=subscriptions.end();
    }
void DeliveryHub::output(const dataflow_runtime::NumericPortFrameView& borrowed) {
        auto found=find_context(borrowed);
        if(found==contexts.end() || (!selected(found->first) &&
            !(borrowed.direction==domain::PortDirection::Input && wled_outputs.contains(borrowed.identity.node))))return;
        core::perf::Scope selected_output_trace(core::perf::Stage::NumericSelectedOutput);
        // Both matrix and vector History publish compact row/lifecycle events through history().
        // Full ring snapshots remain available only through the explicit query boundary.
        // Explicit query_numeric remains the owning full-history snapshot boundary.
        if(const auto* numeric=std::get_if<NumericView>(&borrowed.frame.payload);numeric && numeric->ring.capacity)return;
        auto cached=latest_matrices.find(found->first);
        // Release the prior shared slot before retaining its replacement. Pool
        // exhaustion stays bounded and cannot permanently pin all prior values.
        if(cached!=latest_matrices.end())cached->second={};
        update_wled_input(borrowed);
        if(std::find(subscriptions.begin(),subscriptions.end(),found->first)!=subscriptions.end())queue_preview(found,cached,borrowed);
        if(tap_selected(found->first))queue_tap_frame(found,cached,borrowed);
    }
DeliveryHub::ContextIterator DeliveryHub::find_context(const dataflow_runtime::NumericPortFrameView& borrowed) {
    return std::find_if(contexts.begin(),contexts.end(),[&](const auto& entry) {
        const auto& key=entry.first;return key.node==borrowed.identity.node && key.port==borrowed.identity.port &&
            key.direction==borrowed.direction && key.graph_id==borrowed.graph_id;
    });
}
void DeliveryHub::update_wled_input(const dataflow_runtime::NumericPortFrameView& borrowed) {
    if(borrowed.direction!=domain::PortDirection::Input)return;
    const auto target=wled_outputs.find(borrowed.identity.node);
    if(target==wled_outputs.end())return;
    if(borrowed.frame.stamp.available)target->second.policy.update(borrowed.frame,milliseconds_now());
    else target->second.policy.unavailable();
}
void DeliveryHub::coalesce_preview_frames(const EndpointIdentity& endpoint,const FrameStamp& stamp,Status status) {
    const auto depth=preview_depth_locked();
    std::size_t matching=0;
    const auto count=previews.size();
    for(std::size_t i=0;i<count;++i){auto old=previews.pop();
        if(old && old->context && old->context->endpoint==endpoint)++matching;
        if(old)previews.push(std::move(*old));}
    const bool reset=status!=Status::ok || !stamp.available || stamp.discontinuity;
    for(std::size_t i=0;i<count;++i){auto old=previews.pop();
        if(!old)continue;
        bool discard=!old->context;
        if(old->context && old->context->endpoint==endpoint){
            const auto prior=old->frame?old->frame.view().stamp:FrameStamp{};
            discard=reset || matching>=depth || prior.producer_epoch!=stamp.producer_epoch || prior.generation!=stamp.generation;
            --matching;
        }
        if(discard)++preview_coalesced;else previews.push(std::move(*old));
    }
}
void DeliveryHub::queue_preview(ContextIterator found,LatestIterator cached,const dataflow_runtime::NumericPortFrameView& borrowed) {
    core::perf::Scope preview_produce(core::perf::Stage::NumericPreviewProduce);
    coalesce_preview_frames(found->first,borrowed.frame.stamp,borrowed.status);
    if(previews.size()==128)previews.pop();
    DeliveredNumericFrame preview;
    const auto status=retain_numeric_delivery(found->second,{borrowed.delivery.graph_revision,borrowed.delivery.epoch},
        borrowed.frame,borrowed.status,frame_limits,preview_arena->payload_pool,preview_arena->metadata_pool,preview);
    if(status!=Status::ok)preview={found->second,{borrowed.delivery.graph_revision,borrowed.delivery.epoch},{},status};
    if(cached!=latest_matrices.end())cached->second=preview;
    previews.push(std::move(preview));
    schedule_preview_locked();
}
void DeliveryHub::queue_tap_frame(ContextIterator found,LatestIterator cached,const dataflow_runtime::NumericPortFrameView& borrowed) {
    auto pending_gap=delivery_gaps.find(found->first);
    if(frames.size()==128){++transport_frame_drops;if(pending_gap!=delivery_gaps.end())pending_gap->second=true;return;}
    auto delivered_frame=borrowed.frame;
    delivered_frame.stamp.discontinuity|=(pending_gap!=delivery_gaps.end()&&pending_gap->second);
    DeliveredNumericFrame retained;
    const auto status=retain_numeric_delivery(found->second,{borrowed.delivery.graph_revision,borrowed.delivery.epoch},
        delivered_frame,borrowed.status,frame_limits,arena->payload_pool,arena->metadata_pool,retained);
    if(pending_gap!=delivery_gaps.end())pending_gap->second=status!=Status::ok;
    if(status!=Status::ok)retained={found->second,{borrowed.delivery.graph_revision,borrowed.delivery.epoch},{},status};
    if(cached!=latest_matrices.end())cached->second=retained;
    if(!frames.push(std::move(retained))) {
        ++transport_frame_drops;
        if(pending_gap!=delivery_gaps.end())pending_gap->second=true;
    }
}
void DeliveryHub::history(const dataflow_runtime::NumericHistoryRowView& borrowed) {
        const auto& endpoint=borrowed.endpoint;
        auto found=std::find_if(contexts.begin(),contexts.end(),[&](const auto& entry) {
            const auto& key=entry.first;return key.node==endpoint.identity.node && key.port==endpoint.identity.port &&
                key.direction==endpoint.direction && key.graph_id==endpoint.graph_id;
        });
        if(found==contexts.end())return;
        if(auto ordinal=history_ordinals.find(found->first);ordinal!=history_ordinals.end())ordinal->second=borrowed.accepted_row_ordinal;
        if(!selected(found->first))return;
        NumericHistoryEvent event=NumericHistoryEvent::Unavailable;
        switch(borrowed.event) {
        case dataflow_runtime::NumericHistoryEventKind::Row:event=NumericHistoryEvent::Row;break;
        case dataflow_runtime::NumericHistoryEventKind::Empty:event=NumericHistoryEvent::Empty;break;
        case dataflow_runtime::NumericHistoryEventKind::Reset:event=NumericHistoryEvent::Reset;break;
        case dataflow_runtime::NumericHistoryEventKind::Unavailable:break;
        }
        if(std::find(subscriptions.begin(),subscriptions.end(),found->first)!=subscriptions.end())
            queue_preview_row(found,borrowed,event);
        if(tap_selected(found->first))queue_transport_row(found,borrowed,event);
    }
DeliveredNumericHistoryRow DeliveryHub::retain_history_row(ContextIterator found,
    const dataflow_runtime::NumericHistoryRowView& borrowed,NumericHistoryEvent event,DeliveryArena& storage) {
    const auto& endpoint=borrowed.endpoint;
    DeliveredNumericHistoryRow retained;
    const auto status=retain_numeric_history_row(found->second,{endpoint.delivery.graph_revision,endpoint.delivery.epoch},event,
        borrowed.compact,borrowed.filled_count,borrowed.capacity,borrowed.accepted_row_ordinal,frame_limits,
        storage.payload_pool,storage.metadata_pool,retained);
    if(status!=Status::ok){retained={};retained.event=NumericHistoryEvent::Unavailable;
        retained.compact={found->second,{endpoint.delivery.graph_revision,endpoint.delivery.epoch},{},status};
        retained.capacity=borrowed.capacity;retained.filled_count=borrowed.filled_count;}
    return retained;
}
void DeliveryHub::queue_preview_row(ContextIterator found,const dataflow_runtime::NumericHistoryRowView& borrowed,
                                    NumericHistoryEvent event) {
    if(preview_rows.size()<128)preview_rows.push(retain_history_row(found,borrowed,event,*preview_arena));
    else{
        ++preview_row_drops;preview_rows.pop();DeliveredNumericHistoryRow marker;marker.event=NumericHistoryEvent::Unavailable;
        marker.compact={found->second,{borrowed.endpoint.delivery.graph_revision,borrowed.endpoint.delivery.epoch},{},Status::exhausted};
        marker.capacity=borrowed.capacity;preview_rows.push(std::move(marker));
    }
    schedule_preview_locked();
}
void DeliveryHub::queue_transport_row(ContextIterator found,const dataflow_runtime::NumericHistoryRowView& borrowed,
                                      NumericHistoryEvent event) {
    if(rows.push(retain_history_row(found,borrowed,event,*arena)))return;
    ++transport_row_drops;
    rows.pop();DeliveredNumericHistoryRow marker;marker.event=NumericHistoryEvent::Unavailable;
    marker.compact={found->second,{borrowed.endpoint.delivery.graph_revision,borrowed.endpoint.delivery.epoch},{},Status::exhausted};
    marker.capacity=borrowed.capacity;rows.push(std::move(marker));
}
std::size_t DeliveryHub::preview_depth_locked() const {
        if(subscriptions.empty())return 1;
        return std::max(std::size_t{1},std::min({std::size_t{4},
            std::size_t(16/preview_interval_ms),std::size_t{64}/subscriptions.size()}));
    }
void DeliveryHub::normalize_previews_locked() {
        PreviewSlots queued;
        std::array<bool,128> keep{};
        const auto count=previews.size(),depth=preview_depth_locked();
        for(std::size_t i=0;i<count;++i)queued[i]=previews.pop();
        mark_latest_previews(queued,keep,count,depth,subscriptions);
        restore_previews(queued,keep,count);
    }
void DeliveryHub::mark_latest_previews(const PreviewSlots& queued,std::array<bool,128>& keep,
                                       std::size_t count,std::size_t depth,
                                       const std::vector<EndpointIdentity>& subscriptions) {
        for(std::size_t i=count;i-->0;){
            const auto& frame=queued[i];
            if(!frame || !frame->context)continue;
            const auto& endpoint=frame->context->endpoint;
            if(std::find(subscriptions.begin(),subscriptions.end(),endpoint)==subscriptions.end())continue;
            std::size_t later=0;
            for(std::size_t j=i+1;j<count;++j)
                if(keep[j] && queued[j]->context->endpoint==endpoint)++later;
            keep[i]=later<depth;
        }
    }
void DeliveryHub::restore_previews(PreviewSlots& queued,const std::array<bool,128>& keep,std::size_t count) {
        for(std::size_t i=0;i<count;++i){
            if(keep[i])previews.push(std::move(*queued[i]));else if(queued[i])++preview_coalesced;
        }
    }
void DeliveryHub::schedule_preview_locked() {
        if(preview_scheduled)return;
        preview_scheduled=true;
        const auto queued=steady_ns();
        QMetaObject::invokeMethod(&owner,[this,queued]{
            core::perf::duration(core::perf::Stage::NumericPreviewWake,steady_ns()-queued);
            dispatch_previews();
        },Qt::QueuedConnection);
    }
void DeliveryHub::dispatch_previews() {
        if(!begin_preview_flush())return;
        QPointer<NumericEngine> lifetime(&owner);
        drain_previews();
        if(!lifetime)return;
        std::lock_guard lock(mutex);
        preview_flushing=false;preview_scheduled=false;
        if(previews.size()||preview_rows.size())schedule_preview_locked();
    }
bool DeliveryHub::begin_preview_flush() {
    std::lock_guard lock(mutex);
    if(!preview_scheduled||preview_flushing)return false;
    if(!previews.size()&&!preview_rows.size()){preview_scheduled=false;return false;}
    const auto now=steady_ns();
    const auto interval=std::uint64_t(preview_interval_ms)*1000000;
    const auto deadline=preview_last_flush_ns+interval;
    if(now<deadline){preview_timer_armed_ns=now;preview_timer.start(int((deadline-now+999999)/1000000));return false;}
    preview_last_flush_ns=preview_last_flush_ns && now<deadline+interval?deadline:now;
    preview_flushing=true;
    return true;
}
void DeliveryHub::drain_previews() {
        core::perf::Scope preview_tick(core::perf::Stage::NumericPreviewTick);
        // Copies carry cancellation identity; unsubscribe invalidates already copied work.
        QPointer<NumericEngine> lifetime(&owner);
        drain_preview_frames(lifetime);
        if(lifetime)drain_preview_rows(lifetime);
    }
void DeliveryHub::drain_preview_frames(QPointer<NumericEngine> lifetime) {
        std::array<EndpointIdentity,128> deliveredEndpoints;
        std::size_t deliveredCount=0,frameCount=0;
        {std::lock_guard lock(mutex);frameCount=previews.size();}
        for(std::size_t i=0;i<frameCount;++i) {
            std::optional<DeliveredNumericFrame> next;std::function<void(DeliveredNumericFrame)> handler;
            std::shared_ptr<std::atomic_bool> active;std::uint64_t generation;
            {std::lock_guard lock(mutex);next=previews.pop();generation=delivery_generation;
                if(next && next->context && std::find(deliveredEndpoints.begin(),deliveredEndpoints.begin()+deliveredCount,next->context->endpoint)!=deliveredEndpoints.begin()+deliveredCount){
                    previews.push(std::move(*next));continue;
                }
                if(next && next->context)deliveredEndpoints[deliveredCount++]=next->context->endpoint;
                if(next && next->context && next->delivery.epoch==state.epoch && next->delivery.graph_revision==state.graph_revision &&
                   std::find(subscriptions.begin(),subscriptions.end(),next->context->endpoint)!=subscriptions.end()){handler=frame_handler;active=frame_active;}}
            if(!next)break;
            {std::lock_guard lock(mutex);if(generation!=delivery_generation || next->delivery.epoch!=state.epoch || next->delivery.graph_revision!=state.graph_revision)break;}
            if(handler && active->load(std::memory_order_acquire))handler(*next);
            if(!lifetime)return;
            {std::lock_guard lock(mutex);if(generation!=delivery_generation)break;}
        }

    }
void DeliveryHub::drain_preview_rows(QPointer<NumericEngine> lifetime) {
        for(std::size_t i=0;i<128;++i){
            std::optional<DeliveredNumericHistoryRow> next;std::function<void(DeliveredNumericHistoryRow)> handler;
            std::shared_ptr<std::atomic_bool> active;std::uint64_t generation;
            {std::lock_guard lock(mutex);next=preview_rows.pop();generation=delivery_generation;
                if(next&&next->compact.context&&next->compact.delivery.epoch==state.epoch&&next->compact.delivery.graph_revision==state.graph_revision&&
                   std::find(subscriptions.begin(),subscriptions.end(),next->compact.context->endpoint)!=subscriptions.end()){handler=history_handler;active=history_active;}}
            if(!next)break;
            {std::lock_guard lock(mutex);if(generation!=delivery_generation)break;}
            if(handler&&active->load(std::memory_order_acquire))handler(*next);
            if(!lifetime)return;
        }
    }
void DeliveryHub::drain() {
        QPointer<NumericEngine> lifetime(&owner);
        drain_frames(lifetime);
        if(lifetime)drain_rows(lifetime);
    }
void DeliveryHub::drain_frames(QPointer<NumericEngine> lifetime) {
        for(std::size_t i=0;i<128;++i) {
            std::optional<DeliveredNumericFrame> next;
            struct Pending {std::function<void(DeliveredNumericFrame)> callback;std::shared_ptr<std::atomic_bool> active;};
            std::array<Pending,9> handlers;
            std::uint64_t generation=0;
            std::size_t count=0;
            {std::lock_guard lock(mutex);next=frames.pop();generation=delivery_generation;
                if(next && next->context && next->delivery.epoch==state.epoch && next->delivery.graph_revision==state.graph_revision) {
                    const auto& endpoint=next->context->endpoint;
                    for(const auto& [id,tap]:taps)if(tap.frame && (tap.endpoints.empty() || std::find(tap.endpoints.begin(),tap.endpoints.end(),endpoint)!=tap.endpoints.end()))handlers[count++]={tap.frame,tap.active};

                }
            }
            if(!next)break;
            for(std::size_t h=0;h<count;++h) {
                {std::lock_guard lock(mutex);if(generation!=delivery_generation || next->delivery.epoch!=state.epoch || next->delivery.graph_revision!=state.graph_revision)break;}
                if(handlers[h].active->load(std::memory_order_acquire))handlers[h].callback(*next);
                if(!lifetime)return;
            }
        }
    }
void DeliveryHub::drain_rows(QPointer<NumericEngine> lifetime) {
        for(std::size_t i=0;i<128;++i) {
            std::optional<DeliveredNumericHistoryRow> next;
            struct Pending {std::function<void(DeliveredNumericHistoryRow)> callback;std::shared_ptr<std::atomic_bool> active;};
            std::array<Pending,9> handlers;
            std::uint64_t generation=0;
            std::size_t count=0;
            {std::lock_guard lock(mutex);next=rows.pop();generation=delivery_generation;
                if(next && next->compact.context && next->compact.delivery.epoch==state.epoch && next->compact.delivery.graph_revision==state.graph_revision) {
                    const auto& endpoint=next->compact.context->endpoint;
                    for(const auto& [id,tap]:taps)if(tap.history && (tap.endpoints.empty() || std::find(tap.endpoints.begin(),tap.endpoints.end(),endpoint)!=tap.endpoints.end()))handlers[count++]={tap.history,tap.active};
                }
            }
            if(!next)break;
            for(std::size_t h=0;h<count;++h) {
                {std::lock_guard lock(mutex);if(generation!=delivery_generation || next->compact.delivery.epoch!=state.epoch || next->compact.delivery.graph_revision!=state.graph_revision)break;}
                if(handlers[h].active->load(std::memory_order_acquire))handlers[h].callback(*next);
                if(!lifetime)return;
            }
        }
    }
void DeliveryHub::flush_wled() {
        std::array<WledSend,512> pending;
        const auto count=collect_due_wled(pending);
        for(std::size_t i=0;i<count;++i)send_wled_packet(pending[i]);
    }
std::size_t DeliveryHub::collect_due_wled(std::array<WledSend,512>& pending) {
        std::size_t count=0;
        std::lock_guard lock(mutex);
        for(auto& [node,output]:wled_outputs) {
            wled_protocol::WledPacket packet;if(output.policy.take_due_packet(milliseconds_now(),packet)!=Status::ok)continue;
            pending[count++]={node,output.socket,output.destination,output.interface,output.port,packet};
        }
        return count;
    }
void DeliveryHub::send_wled_packet(WledSend& output) {
        const bool bound=output.socket->state()!=QAbstractSocket::UnconnectedState ||
                         output.socket->bind(output.interface,quint16(0));
        const bool sent=bound && output.socket->writeDatagram(reinterpret_cast<const char*>(output.packet.data()),
            output.packet.size(),output.destination,output.port)==qint64(output.packet.size());
        std::lock_guard lock(mutex);const auto current=wled_outputs.find(output.node);
        if(current==wled_outputs.end() || current->second.socket!=output.socket)return;
        current->second.policy.note_send_result(sent);
        if(!sent)state.statuses[QStringLiteral("wled")]={State::Failed,output.socket->errorString(),state.revision};
    }
}
