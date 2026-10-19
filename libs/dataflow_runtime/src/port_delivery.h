#pragma once
#include "execution_measurement.h"
namespace dataflow_runtime::detail {
// Synchronous borrowed delivery; no buffer or frame ownership crosses this helper.
class PortDelivery {
    const NumericRuntimeProfile& profile;
    std::span<const RoutingAlias> aliases;
    const NumericRuntimeCallbacks callbacks;
    std::uint64_t& child_time;
    template<class Callback,class Event> void deliver(Callback callback,const Event& event) noexcept {
        if(!callback)return;
        const auto begin=measurement_now(profile);callback(callbacks.user,event);const auto end=measurement_now(profile);
        if(end>=begin)child_time+=end-begin;
    }
    void output_alias(const NumericPortFrameView& event,const RoutingAlias& alias) noexcept {
        auto routed=event;routed.identity={alias.node,alias.port};routed.kind=kind_id(alias.kind);
        if(alias.include_input){routed.direction=PortDirection::Input;deliver(callbacks.sink_input,routed);}
        routed.direction=alias.direction;deliver(alias.direction==PortDirection::Input?callbacks.sink_input:callbacks.output,routed);
    }
    void history_alias(const NumericHistoryRowView& event,const RoutingAlias& alias) noexcept {
        auto routed=event;routed.endpoint.identity={alias.node,alias.port};routed.endpoint.kind=kind_id(alias.kind);
        if(alias.include_input){routed.endpoint.direction=PortDirection::Input;deliver(callbacks.history,routed);}
        routed.endpoint.direction=alias.direction;deliver(callbacks.history,routed);
    }
public:
    PortDelivery(const NumericRuntimeProfile& runtime,std::span<const RoutingAlias> routing,NumericRuntimeCallbacks sink,std::uint64_t& elapsed) noexcept
        :profile(runtime),aliases(routing),callbacks(sink),child_time(elapsed){}
    void output(const NumericPortFrameView& event,bool internal) noexcept {
        if(!internal)deliver(callbacks.output,event);
        for(const auto& alias:aliases)if(alias.source==event.identity.node && alias.source_port==event.identity.port)output_alias(event,alias);
    }
    void sink(const NumericPortFrameView& event) noexcept {deliver(callbacks.sink_input,event);}
    void history(const NumericHistoryRowView& event) noexcept {
        deliver(callbacks.history,event);
        if(event.endpoint.direction!=PortDirection::Output)return;
        for(const auto& alias:aliases)if(alias.source==event.endpoint.identity.node && alias.source_port==event.endpoint.identity.port)history_alias(event,alias);
    }
};
}
