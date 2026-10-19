#include "ingress_acceptance.h"
#include "frame_access.h"
#include "preparation_support.h"
#include <wled_protocol/wled_packet.h>
#include <algorithm>
namespace dataflow_runtime::detail {
IngressAcceptance::IngressAcceptance(std::span<const PlanNode> plan,std::span<State> state,std::size_t source,PortId port,DataFrameView& frame,const FrameLimits& bounds,const OwnedDataFrame* owner,std::optional<std::uint64_t> ordinal,std::shared_ptr<const void>& retention,std::optional<std::uint64_t> filled) noexcept
    :nodes(plan),states(state),source_index(source),ingress_port(port),input(frame),limits(bounds),owned(owner),history_ordinal(ordinal),retention_owner(retention),history_filled_count(filled),source_node(nodes[source]),source_state(states[source]){}
Status IngressAcceptance::declare_history() noexcept {
    history_input=std::get_if<NumericContract>(&*actual);
    external_history=history_input && (std::get<NumericView>(input.payload).ring.capacity!=0 || (history_ordinal && (history_input->shape.rank==Rank::Matrix || history_input->semantic==Semantic::History)));
    if(!external_history)return Status::ok;
    if(!owned || !retention_owner || !history_ordinal || (input.stamp.available && !std::get<NumericView>(input.payload).ring.capacity))return Status::invalid_argument;
    real_filled=history_filled_count.value_or(input.stamp.available?std::get<NumericView>(input.payload).actual[0]:source_state.progress);
    const auto capacity=input.stamp.available?std::get<NumericView>(input.payload).actual[0]:history_input->shape.extents[0].maximum;
    return real_filled>capacity || real_filled>*history_ordinal?Status::invalid_argument:Status::ok;
}
Status IngressAcceptance::validate_payload() const noexcept {
    const auto status=validate_view(input,limits);if(status!=Status::ok && status!=Status::unavailable)return status;
    if(!input.stamp.available)return Status::ok;
    if(const auto* value=std::get_if<NumericView>(&input.payload))return validate_values(*value,ValueDomain::AnyIeee,limits);
    const auto& structured=std::get<StructuredView>(input.payload);
    if(packet_contract(*structured.contract) && wled_protocol::validate_packet({reinterpret_cast<const std::uint8_t*>(structured.bytes.data()),structured.bytes.size()},{0,wled_protocol::packet_size})!=wled_protocol::Status::Ok)return Status::invalid_argument;
    return Status::ok;
}
Status IngressAcceptance::validate_input() noexcept {
    actual=std::visit([](const auto& value)->std::optional<DataContract>{return value.contract?std::optional<DataContract>{*value.contract}:std::nullopt;},input.payload);
    if(!actual || !same_physical_contract(*actual,source_node.numeric_outputs[ingress_port]))return Status::incompatible_contract;
    {const auto status=declare_history();if(status!=Status::ok)return status;}
    {const auto status=validate_payload();if(status!=Status::ok)return status;}
    if(history_input && history_input->shape.rank==Rank::Vector && history_input->shape.axes[0]==Axis::Sample && input.stamp.available && input.stamp.has_sample_interval && input.stamp.sample_count!=std::get<NumericView>(input.payload).actual[0])return Status::incompatible_contract;
    return Status::ok;
}
void IngressAcceptance::detect_packet_gap(const FrameStamp& old) noexcept {
    if(!history_input || history_input->shape.rank!=Rank::Vector || history_input->shape.axes[0]!=Axis::Sample || !input.stamp.available || !old.available || !input.stamp.has_sample_interval || !old.has_sample_interval || input.stamp.producer_epoch!=old.producer_epoch || input.stamp.generation!=old.generation)return;

            if(old.sample_count>std::uint64_t(INT64_MAX) || old.sample_begin>INT64_MAX-std::int64_t(old.sample_count) ||
               input.stamp.sample_begin!=old.sample_begin+std::int64_t(old.sample_count))input.stamp.discontinuity=true;

}
Status IngressAcceptance::validate_filled_progress(const NumericView& incoming,std::uint64_t delta) const noexcept {
    const auto expected=source_state.progress+std::min<std::uint64_t>(delta,incoming.ring.capacity-source_state.progress);
    return incoming.actual[0]!=incoming.ring.capacity || real_filled!=expected || incoming.ring.oldest!=(source_state.oldest+delta%incoming.ring.capacity)%incoming.ring.capacity?Status::incompatible_contract:Status::ok;
}
Status IngressAcceptance::validate_growing_progress(const NumericView& incoming,std::uint64_t delta) const noexcept {
    const auto free=incoming.ring.capacity-source_state.frames;
    const auto overwritten=delta>free?delta-free:0;
    const auto expected=delta>=free?incoming.ring.capacity:source_state.frames+std::size_t(delta);
    return incoming.actual[0]!=expected || incoming.ring.oldest!=(source_state.oldest+overwritten%incoming.ring.capacity)%incoming.ring.capacity?Status::incompatible_contract:Status::ok;
}
Status IngressAcceptance::validate_history_progress(const FrameStamp& old) const noexcept {
    if(!external_history || !source_state.history_origin_valid || input.stamp.producer_epoch!=old.producer_epoch || input.stamp.generation!=old.generation || input.stamp.discontinuity)return Status::ok;
    if(*history_ordinal<source_state.row_ordinal)return Status::invalid_argument;
    if(!input.stamp.available)return Status::ok;
    const auto& incoming=std::get<NumericView>(input.payload);const auto delta=*history_ordinal-source_state.row_ordinal;
    return history_filled_count?validate_filled_progress(incoming,delta):validate_growing_progress(incoming,delta);
}
Status IngressAcceptance::validate_marker(const FrameStamp& old) const noexcept {
        if(input.stamp.producer_epoch==old.producer_epoch && (input.stamp.generation<old.generation ||
            (input.stamp.generation==old.generation && (input.stamp.available || input.stamp.sequence) && input.stamp.sequence<=old.sequence)))return Status::would_block;
        for(std::size_t i=0;i<source_state.retired_count;++i)if(source_state.retired[i]==input.stamp.producer_epoch)return Status::would_block;
    return Status::ok;
}
bool IngressAcceptance::belongs_to_lane(std::size_t branch) const noexcept {
    while(nodes[branch].parent!=absent && nodes[branch].parent!=source_index)branch=nodes[branch].parent;
    return nodes[branch].parent!=source_index || nodes[branch].parent_port==ingress_port;
}
void IngressAcceptance::reset_descendant(std::size_t index) noexcept {
    // History retains rows. A Fourier join retains a pending opposite plane;
    // accept_fourier fences the new interval/epoch before publishing a pair.
    if(nodes[index].operation==ProcessingOperation::History || paired_processing(nodes[index].operation))states[index].pending_discontinuity=true;
    else clear_node_state(nodes[index],states[index]);
}
void IngressAcceptance::retire_epoch(std::uint64_t epoch) noexcept {
    if(input.stamp.producer_epoch==epoch)return;
    source_state.retired[source_state.retired_next]=epoch;
    source_state.retired_next=(source_state.retired_next+1)%source_state.retired.size();
    source_state.retired_count=std::min(source_state.retired_count+1,source_state.retired.size());
}
void IngressAcceptance::reset_descendants(const FrameStamp& old) noexcept {
    if(input.stamp.producer_epoch==old.producer_epoch && input.stamp.generation==old.generation && !input.stamp.discontinuity)return;
    for(std::size_t index=0;index<nodes.size();++index)
        if(nodes[index].clock==source_index && belongs_to_lane(index))reset_descendant(index);
    retire_epoch(old.producer_epoch);
}
Status IngressAcceptance::retain_history() noexcept {
    if(external_history){
        source_state.history_origin_valid=true;
        if(!input.stamp.available){source_state.frames=history_input->shape.extents[0].maximum;source_state.progress=std::size_t(real_filled);source_state.row_ordinal=*history_ordinal;}
        if(*history_ordinal<source_state.row_ordinal)return Status::invalid_argument;
        auto previous_owner=source_state.ingress_owner;source_state.ingress_owner=std::move(retention_owner);source_state.owned_ingress=*owned;
    }
    if(!external_history && input.stamp.available && history_input && (history_input->shape.rank==Rank::Matrix || history_input->semantic==Semantic::History)){
        source_state.owned_ingress={};source_state.ingress_owner.reset();source_state.history_origin_valid=false;
        source_state.frames=source_state.oldest=source_state.progress=0;source_state.row_ordinal=0;
    }
    return Status::ok;
}
Status IngressAcceptance::accept() noexcept {
    {const auto status=validate_input();if(status!=Status::ok)return status;}
    if(source_state.source_markers[ingress_port]){
        const auto& old=source_state.source_stamps[ingress_port];detect_packet_gap(old);
        {const auto status=validate_history_progress(old);if(status!=Status::ok)return status;}
        {const auto status=validate_marker(old);if(status!=Status::ok)return status;}
        reset_descendants(old);
    }
    return retain_history();
}
}
